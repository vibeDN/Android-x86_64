// SPDX-License-Identifier: GPL-2.0-or-later
// armtr NativeBridge (callbacks v8): runs arm64-v8a JNI libraries inside an x86_64 app process.
// Design: docs/translator/05-native-bridge.md (in short):
//  * initialize() boots a guest process in-process: the real guest linker64 loads /system/bin/armtr/armtr_guest,
//    which initialises the guest libc once and parks the thread with a private syscall (ARMTR_NR_READY).
//  * loadLibrary/dlsym/namespaces call the guest linker's exported __loader_* entry points on the engine.
//  * getTrampoline* return host thunks (jni_thunks_x86_64.S) that marshal a SysV JNI call by shorty into
//    AAPCS64, swap the JNIEnv*/JavaVM* for guest-visible proxies (proxy module) and run the guest function.
//  * Host threads that call into the guest without a guest CPU are attached lazily: a CPU plus guest TLS made by
//    the guest's own pthread_create, whose clone is captured onto the calling thread (no new thread starts).
//  * RegisterNatives from guest code reaches ART with guest pointers; ART asks isNativeBridgeFunctionPointer /
//    getTrampolineForFunctionPointer (guest executable mappings are tracked from mmap/mprotect/munmap).
#define LOG_TAG "armtr"

#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "armtr_loader_ext.h"
#include "guest_rt.h"
#include "hostlibs.h"
#include "nativebridge/native_bridge.h"
#include "proxy_ext.h"

#ifdef __ANDROID__
#include <log/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) (getenv("ARMTR_VERBOSE") ? (fprintf(stderr, "armtr: " __VA_ARGS__), fputc('\n', stderr)) : 0)
#define LOGE(...) (fprintf(stderr, "armtr: E " __VA_ARGS__), fputc('\n', stderr))
#endif

// Layout is private to ART (art/libnativebridge/native_bridge.cc) and not in the public header.
namespace android {
struct NativeBridgeRuntimeValues {
  const char* os_arch;
  const char* cpu_abi;
  const char* cpu_abi2;
  const char* const* supported_abis;
  int32_t abi_count;
};
}  // namespace android

extern char** environ;
extern "C" char armtr_jni_thunks[];   // jni_thunks_x86_64.S
extern "C" uint64_t armtr_jni_dispatch(uint32_t idx, const uint64_t* gpr, const uint64_t* xmm, const uint64_t* stk);

namespace {

constexpr uint32_t kVersion = 8;
constexpr const char* kGuestIsa = "arm64";
constexpr const char* kSupportedPathSubstring = "/lib/arm64";   // lib/arm64/x.so and base.apk!/lib/arm64-v8a/x.so
constexpr const char* kGuestExe = "/system/bin/armtr/armtr_guest";
constexpr const char* kGuestLinker = "/system/bin/armtr/linker64";
constexpr uint64_t kNrReady = 0x4152;          // bridge/guest/armtr_guest.c
constexpr uint64_t kNrResolve = 0x4153;        // bridge/guest/gen_stubs.py: x16 = {target, "lib.so", "symbol"}
constexpr uint64_t kNrResolveData = 0x4154;
constexpr size_t kMainStack = 8 << 20, kThreadStack = 4 << 20;
constexpr uint64_t kNeverAddr = 0x1000;
constexpr uint64_t kDlextUseNamespace = 0x200;   // ANDROID_DLEXT_USE_NAMESPACE
constexpr int kThunks = 16384;                   // ARMTR_JNI_THUNKS

const android::NativeBridgeRuntimeValues kRuntimeValues = {"aarch64", "arm64-v8a", nullptr, nullptr, 0};

struct Guest {
  bool booted = false, ready = false;
  armtr_cpu* main_cpu = nullptr;
  uint64_t main_tls = 0;
  uint64_t caller = 0;   // an address inside the guest exe: caller_addr for __loader_* (=> default namespace)
  uint64_t dlopen_ext = 0, dlsym = 0, dlerror = 0, create_ns = 0, link_ns = 0, exported_ns = 0, anon_ns = 0;
  uint64_t pthread_create = 0;
} g;
std::mutex g_mu;                 // boot + attach
thread_local std::string t_error = "armtr: not initialized";

// ---- guest executable ranges (isNativeBridgeFunctionPointer) ----
std::mutex g_x_mu;
std::map<uint64_t, uint64_t> g_exec;   // start -> end
void set_exec(uint64_t b, uint64_t len, bool x) {
  uint64_t e = b + ((len + 4095) & ~4095ull);
  std::lock_guard<std::mutex> l(g_x_mu);
  auto it = g_exec.lower_bound(b);
  if (it != g_exec.begin() && std::prev(it)->second > b) --it;
  while (it != g_exec.end() && it->first < e) {
    uint64_t s = it->first, t = it->second;
    it = g_exec.erase(it);
    if (s < b) g_exec[s] = b;
    if (t > e) g_exec[e] = t;
  }
  if (x) g_exec[b] = e;
}
bool is_guest_code(const void* p) {
  uint64_t a = (uint64_t)p;
  std::lock_guard<std::mutex> l(g_x_mu);
  auto it = g_exec.upper_bound(a);
  if (it == g_exec.begin()) return false;
  return a < (--it)->second;
}

// A host-library function nobody provides: report it once per call site name and return 0.
void missing_cb(armtr_cpu* cpu, void* user) {
  LOGE("guest called unavailable host function %s", (const char*)user);
  armtr_regs r; armtr_regs_get(cpu, &r);
  r.x[0] = 0;
  armtr_regs_set(cpu, &r);
}

// Guest proxy-library stub (gen_stubs.py) asks for its host function: store the hostlibs proxy in desc[0].
void resolve_stub(armtr_cpu* cpu, bool data) {
  armtr_regs r; armtr_regs_get(cpu, &r);
  uint64_t* desc = (uint64_t*)r.x[16];
  const char *lib = (const char*)desc[1], *sym = (const char*)desc[2];
  uint64_t a = armtr_hostlibs_resolve2(lib, sym);
  if (data) { *(uint64_t*)desc[0] = a ? *(uint64_t*)a : 0; return; }
  if (!a) {
    LOGE("%s: %s not available on the host (calls return 0)", lib, sym);
    a = armtr_proxy_make_raw(nullptr, missing_cb, (void*)sym);
  }
  desc[0] = a;
}

// Embedder syscall hook (runs inside the syscall intercept: no guest calls here).
int64_t hook(armtr_cpu* cpu, uint64_t nr, const uint64_t a[6], void*) {
  switch (nr) {
    case kNrReady:
      g.ready = true;
      armtr_stop(cpu);   // park the guest main thread inside main()
      return 0;
    case kNrResolve:
    case kNrResolveData:
      resolve_stub(cpu, nr == kNrResolveData);
      return (int64_t)a[0];   // x0 is the stub's first argument: keep it
    case 222: {   // mmap
      int64_t r = armtr_syscall(cpu, nr, a, nullptr);
      if (r >= 0) set_exec((uint64_t)r, a[1], a[2] & 4);
      return r;
    }
    case 226: {   // mprotect
      int64_t r = armtr_syscall(cpu, nr, a, nullptr);
      if (r == 0) set_exec(a[0], a[1], a[2] & 4);
      return r;
    }
    case 215: {   // munmap
      int64_t r = armtr_syscall(cpu, nr, a, nullptr);
      if (r == 0) set_exec(a[0], a[1], false);
      return r;
    }
    default: return ARMTR_SYSCALL_PASS;
  }
}

std::string host_path(const char* guest) {
  std::string h;
  return armtr_rt::map_path(guest, &h) ? h : std::string(guest);
}

// st_value of a dynamic symbol in an ELF file (the guest linker exports __loader_*), 0 if absent.
uint64_t elf_sym(const std::string& path, const char* name) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  std::vector<char> f;
  off_t n = lseek(fd, 0, SEEK_END);
  if (n > 0) { f.resize((size_t)n); if (pread(fd, f.data(), f.size(), 0) != n) f.clear(); }
  close(fd);
  if (f.size() < sizeof(Elf64_Ehdr)) return 0;
  auto* eh = (const Elf64_Ehdr*)f.data();
  if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > f.size()) return 0;
  auto* sh = (const Elf64_Shdr*)(f.data() + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
    const Elf64_Shdr& st = sh[sh[i].sh_link];
    auto* sym = (const Elf64_Sym*)(f.data() + sh[i].sh_offset);
    for (size_t k = 0; k < sh[i].sh_size / sizeof(Elf64_Sym); k++)
      if (sym[k].st_name < st.sh_size && !strcmp(f.data() + st.sh_offset + sym[k].st_name, name)) return sym[k].st_value;
  }
  return 0;
}

[[noreturn]] void guest_crash(armtr_cpu* cpu, const char* what) {
  std::string d = armtr_rt::describe_fault(cpu);
  LOGE("guest fault in %s: %s", what, d.c_str());
#ifdef __ANDROID__
  LOG_ALWAYS_FATAL("armtr: guest fault in %s: %s", what, d.c_str());
#else
  abort();
#endif
}

armtr_cpu* cpu_for_thread();

// Integer-only guest call on this thread's CPU.
uint64_t gcall(uint64_t fn, std::initializer_list<uint64_t> args, const char* what) {
  armtr_cpu* cpu = cpu_for_thread();
  if (!cpu) { LOGE("no guest cpu for %s", what); return 0; }
  uint64_t x[8] = {0}, r = 0;
  int n = 0;
  for (uint64_t v : args) x[n++] = v;
  if (armtr_rt::call(cpu, fn, x, n, nullptr, 0, nullptr, 0, &r) < 0) guest_crash(cpu, what);
  return r;
}

// A host thread without a guest CPU: create one and give it guest TLS from the guest's pthread_create, whose
// clone is captured onto this thread. The pthread_create call itself borrows the main thread's TLS (serialised).
// ponytail: the guest thread record + its unused 1 MiB guest stack are leaked when the host thread exits.
armtr_cpu* cpu_for_thread() {
  if (armtr_cpu* c = armtr_rt::thread_cpu()) return c;
  if (!g.ready) return nullptr;
  std::lock_guard<std::mutex> l(g_mu);
  armtr_cpu* c = armtr_cpu_create_ex(armtr_syscall, nullptr, kThreadStack);
  if (!c) return nullptr;
  armtr_rt::set_thread_cpu(c);
  armtr_regs r; armtr_regs_get(c, &r);
  r.tpidr_el0 = g.main_tls;
  armtr_regs_set(c, &r);
  armtr_regs child; memset(&child, 0, sizeof child);
  armtr_rt::capture_next_clone(&child);
  uint64_t th = 0, rc = 0, x[4] = {(uint64_t)&th, 0, g.caller, 0};
  if (armtr_rt::call(c, g.pthread_create, x, 4, nullptr, 0, nullptr, 0, &rc) < 0) guest_crash(c, "thread attach");
  armtr_rt::capture_next_clone(nullptr);
  if (rc != 0 || !child.tpidr_el0) {
    LOGE("thread attach: guest pthread_create failed (%d)", (int)rc);
    armtr_rt::set_thread_cpu(nullptr);
    armtr_cpu_destroy(c);
    return nullptr;
  }
  armtr_regs_get(c, &r);
  r.tpidr_el0 = child.tpidr_el0;
  armtr_regs_set(c, &r);
  LOGI("attached host thread %d (guest tls 0x%llx)", gettid(), (unsigned long long)child.tpidr_el0);
  return c;
}

bool boot_guest() {
  std::lock_guard<std::mutex> l(g_mu);
  if (g.booted) return g.ready;
  g.booted = true;
  const char* root = getenv("ARMTR_SYSROOT");   // host tests: guest root laid out like the device
  armtr_rt::set_sysroot(root ? root : "");
  armtr_rt::set_trace(getenv("ARMTR_TRACE") != nullptr);
  armtr_rt::set_proc_self_exe(kGuestExe);
  armtr_rt::set_embedder_hook(hook);
  if (const char* d = getenv("ARMTR_HOSTLIBS_DIR")) armtr_hostlibs_set_dir(d);   // host tests: fake platform libs
  if (armtr_rt::init()) { t_error = "armtr: engine init failed"; return false; }
  armtr_cpu* cpu = armtr_cpu_create_ex(armtr_syscall, nullptr, kMainStack);
  if (!cpu) { t_error = "armtr: cannot create cpu"; return false; }
  std::vector<std::string> env;
  for (char** e = environ; e && *e; e++)
    if (strncmp(*e, "LD_", 3)) env.push_back(*e);   // host loader settings mean nothing to the guest linker
  armtr_rt::Exec ex;
  std::string err;
  if (!armtr_rt::setup_exec(cpu, host_path(kGuestExe), kGuestExe, {kGuestExe}, env, &ex, &err)) {
    t_error = "armtr: " + err;
    return false;
  }
  armtr_rt::set_thread_cpu(cpu);
  armtr_rt::run_depth()++;
  int rc = armtr_run(cpu, kNeverAddr);
  armtr_rt::run_depth()--;
  if (!g.ready) {
    t_error = rc < 0 ? "armtr: guest boot: " + armtr_rt::describe_fault(cpu) : "armtr: guest exited during boot";
    armtr_rt::set_thread_cpu(nullptr);
    return false;
  }
  armtr_regs r; armtr_regs_get(cpu, &r);
  g.main_cpu = cpu;
  g.main_tls = r.tpidr_el0;
  armtr_rt::set_call_stack_top(r.sp - 1024);   // keep the parked main() frame, argv/env/auxv intact
  g.caller = armtr_image_entry(ex.exe);
  uint64_t lb = armtr_image_base(ex.interp);
  std::string lp = host_path(kGuestLinker);
  set_exec(armtr_image_base(ex.exe), armtr_image_size(ex.exe), true);
  set_exec(lb, armtr_image_size(ex.interp), true);
  struct { uint64_t* p; const char* n; } syms[] = {
      {&g.dlopen_ext, "__loader_android_dlopen_ext"}, {&g.dlsym, "__loader_dlsym"},
      {&g.dlerror, "__loader_dlerror"}, {&g.create_ns, "__loader_android_create_namespace"},
      {&g.link_ns, "__loader_android_link_namespaces"}, {&g.exported_ns, "__loader_android_get_exported_namespace"},
      {&g.anon_ns, "__loader_android_init_anonymous_namespace"}};
  for (auto& s : syms) {
    uint64_t v = elf_sym(lp, s.n);
    if (!v) { t_error = std::string("armtr: guest linker lacks ") + s.n; g.ready = false; return false; }
    *s.p = lb + v;
  }
  uint64_t x[3] = {0 /*RTLD_DEFAULT*/, (uint64_t)"pthread_create", g.caller}, pc = 0;
  if (armtr_rt::call(cpu, g.dlsym, x, 3, nullptr, 0, nullptr, 0, &pc) < 0 || !pc) {
    t_error = "armtr: guest pthread_create not found";
    g.ready = false;
    return false;
  }
  g.pthread_create = pc;
  LOGI("guest booted: linker@0x%llx tls=0x%llx", (unsigned long long)lb, (unsigned long long)g.main_tls);
  return true;
}

void set_guest_error(const char* what) {
  uint64_t e = gcall(g.dlerror, {}, "dlerror");
  t_error = std::string(what) + ": " + (e ? (const char*)e : "unknown guest linker error");
}

// ---- JNI thunks ----
enum Kind : uint8_t { kRegular, kCritical, kOnLoad };
struct Thunk { uint64_t guest; Kind kind; char shorty[64]; };
Thunk g_thunks[kThunks];
std::atomic<int> g_nthunks{0};
std::mutex g_th_mu;
std::map<std::string, int> g_thunk_ids;

void* make_thunk(uint64_t guest, const char* shorty, Kind kind) {
  if (!shorty) shorty = "";
  if (strlen(shorty) >= sizeof(Thunk::shorty)) { LOGE("shorty too long: %s", shorty); return nullptr; }
  std::string key = std::to_string(guest) + ":" + std::to_string(kind) + ":" + shorty;
  std::lock_guard<std::mutex> l(g_th_mu);
  auto it = g_thunk_ids.find(key);
  int i;
  if (it != g_thunk_ids.end()) i = it->second;
  else {
    i = g_nthunks.load();
    if (i >= kThunks) { LOGE("out of JNI thunks"); return nullptr; }
    g_thunks[i].guest = guest; g_thunks[i].kind = kind;
    strcpy(g_thunks[i].shorty, shorty);
    g_nthunks.store(i + 1);   // publish after the entry is complete
    g_thunk_ids[key] = i;
  }
  return armtr_jni_thunks + 16 * i;
}

uint64_t norm(char c, uint64_t v) {
  switch (c) {
    case 'Z': return (uint8_t)v;
    case 'B': return (uint64_t)(int64_t)(int8_t)v;
    case 'C': return (uint16_t)v;
    case 'S': return (uint64_t)(int64_t)(int16_t)v;
    case 'I': return (uint64_t)(int64_t)(int32_t)v;
    case 'F': return (uint32_t)v;
    default: return v;
  }
}

// ---- callbacks ----
const android::NativeBridgeRuntimeCallbacks* g_runtime = nullptr;

bool nb_initialize(const android::NativeBridgeRuntimeCallbacks* cbs, const char* private_dir, const char* isa) {
  LOGI("initialize(private_dir=%s, isa=%s)", private_dir ? private_dir : "(null)", isa ? isa : "(null)");
  if (!isa || strcmp(isa, kGuestIsa) != 0) return false;
  g_runtime = cbs;
  if (!boot_guest()) { LOGE("%s", t_error.c_str()); return false; }
  return true;
}

void* nb_loadLibraryExt(const char* path, int flag, android::native_bridge_namespace_t* ns) {
  if (!g.ready) { LOGE("loadLibrary(%s): %s", path ? path : "", t_error.c_str()); return nullptr; }
  struct { uint64_t flags, reserved_addr, reserved_size; int32_t relro_fd, library_fd; int64_t fd_offset;
           uint64_t ns; } ext = {kDlextUseNamespace, 0, 0, -1, -1, 0, (uint64_t)ns};
  uint64_t h = gcall(g.dlopen_ext, {(uint64_t)path, (uint64_t)(uint32_t)flag, ns ? (uint64_t)&ext : 0, g.caller},
                     "dlopen");
  if (!h) set_guest_error("dlopen failed");
  LOGI("loadLibraryExt(%s, 0x%x, ns=%p) -> 0x%llx%s%s", path ? path : "", flag, ns, (unsigned long long)h,
       h ? "" : " ", h ? "" : t_error.c_str());
  return (void*)h;
}
void* nb_loadLibrary(const char* path, int flag) { return nb_loadLibraryExt(path, flag, nullptr); }

void* nb_getTrampolineWithJNICallType(void* handle, const char* name, const char* shorty, uint32_t,
                                      android::JNICallType type) {
  if (!g.ready || !name) return nullptr;
  uint64_t a = gcall(g.dlsym, {(uint64_t)handle, (uint64_t)name, g.caller}, "dlsym");
  void* t = nullptr;
  if (a) {
    if (!strcmp(name, "JNI_OnLoad")) t = make_thunk(a, "I", kOnLoad);
    else if (shorty) t = make_thunk(a, shorty, type == android::kJNICallTypeCriticalNative ? kCritical : kRegular);
    else LOGE("getTrampoline(%s): no shorty for a non-JNI symbol (unsupported)", name);
  }
  LOGI("getTrampoline(%p, %s, %s, %d) -> guest 0x%llx thunk %p", handle, name, shorty ? shorty : "(null)", (int)type,
       (unsigned long long)a, t);
  return t;
}
void* nb_getTrampoline(void* handle, const char* name, const char* shorty, uint32_t len) {
  return nb_getTrampolineWithJNICallType(handle, name, shorty, len, android::kJNICallTypeRegular);
}
void* nb_getTrampolineForFunctionPointer(const void* method, const char* shorty, uint32_t,
                                         android::JNICallType type) {
  if (!is_guest_code(method)) return const_cast<void*>(method);   // a host function registered for a bridged class
  return make_thunk((uint64_t)method, shorty, type == android::kJNICallTypeCriticalNative ? kCritical : kRegular);
}
bool nb_isNativeBridgeFunctionPointer(const void* method) { return is_guest_code(method); }

bool nb_isSupported(const char* path) {
  if (!path) return false;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return true;   // bare soname: decided by the guest linker
  Elf64_Ehdr eh;
  bool ok = pread(fd, &eh, sizeof eh, 0) == (ssize_t)sizeof eh && !memcmp(eh.e_ident, ELFMAG, 4) &&
            eh.e_machine == EM_AARCH64;
  close(fd);
  return ok;
}
bool nb_isPathSupported(const char* path) { return path && strstr(path, kSupportedPathSubstring); }
const android::NativeBridgeRuntimeValues* nb_getAppEnv(const char* isa) {
  return (isa && !strcmp(isa, kGuestIsa)) ? &kRuntimeValues : nullptr;
}
bool nb_isCompatibleWith(uint32_t v) { return v >= 2 && v <= kVersion; }
// Engine faults are handled by the engine's own chained SIGSEGV/SIGBUS handler (registered through
// libsigchain's sigaction, so ART's fault manager runs first and passes non-managed faults on).
android::NativeBridgeSignalHandlerFn nb_getSignalHandler(int) { return nullptr; }
int nb_unloadLibrary(void*) { return 0; }   // like Berberis: never unloaded
const char* nb_getError() { return t_error.c_str(); }

bool nb_initAnonymousNamespace(const char* sonames, const char* path) {
  if (!g.ready) return false;
  return (uint8_t)gcall(g.anon_ns, {(uint64_t)sonames, (uint64_t)path}, "init_anonymous_namespace");
}
android::native_bridge_namespace_t* nb_createNamespace(const char* name, const char* ld_path, const char* default_path,
                                                       uint64_t type, const char* permitted,
                                                       android::native_bridge_namespace_t* parent) {
  if (!g.ready) return nullptr;
  uint64_t ns = gcall(g.create_ns, {(uint64_t)name, (uint64_t)ld_path, (uint64_t)default_path, type,
                                    (uint64_t)permitted, (uint64_t)parent, g.caller}, "create_namespace");
  if (!ns) set_guest_error("create_namespace failed");
  LOGI("createNamespace(%s, default=%s, type=0x%llx, parent=%p) -> 0x%llx", name ? name : "",
       default_path ? default_path : "", (unsigned long long)type, parent, (unsigned long long)ns);
  return (android::native_bridge_namespace_t*)ns;
}
bool nb_linkNamespaces(android::native_bridge_namespace_t* from, android::native_bridge_namespace_t* to,
                       const char* sonames) {
  if (!g.ready) return false;
  bool ok = (uint8_t)gcall(g.link_ns, {(uint64_t)from, (uint64_t)to, (uint64_t)sonames}, "link_namespaces");
  if (!ok) set_guest_error("link_namespaces failed");
  return ok;
}
android::native_bridge_namespace_t* nb_getExportedNamespace(const char* name) {
  if (!g.ready || !name) return nullptr;
  return (android::native_bridge_namespace_t*)gcall(g.exported_ns, {(uint64_t)name}, "get_exported_namespace");
}
android::native_bridge_namespace_t* nb_getVendorNamespace() { return nb_getExportedNamespace("sphal"); }
void nb_preZygoteFork() {}   // the bridge is only initialised in app processes, never in a zygote

}  // namespace

// Called by armtr_jni_common for thunk `idx` with the saved SysV argument registers.
extern "C" __attribute__((visibility("hidden"))) uint64_t armtr_jni_dispatch(uint32_t idx, const uint64_t* gpr,
                                                                          const uint64_t* xmm, const uint64_t* stk) {
  const Thunk& t = g_thunks[idx];
  armtr_cpu* cpu = cpu_for_thread();
  if (!cpu) {
    LOGE("JNI call 0x%llx: no guest cpu", (unsigned long long)t.guest);
    abort();
  }
  uint64_t x[8], v[8], s[64];
  int nx = 0, nv = 0, ns = 0, hi = 0, hf = 0, hs = 0;
  auto host_int = [&]() { return hi < 6 ? gpr[hi++] : stk[hs++]; };
  auto host_fp = [&]() { return hf < 8 ? xmm[hf++] : stk[hs++]; };
  auto put = [&](bool fp, uint64_t val) {
    if (fp) { if (nv < 8) v[nv++] = val; else s[ns++] = val; }
    else    { if (nx < 8) x[nx++] = val; else s[ns++] = val; }
  };
  const char* args = t.shorty[0] ? t.shorty + 1 : "";
  if (t.kind == kOnLoad) {   // jint JNI_OnLoad(JavaVM*, void*)
    put(false, armtr_proxy_javavm((void*)host_int()));
    put(false, host_int());
  } else if (t.kind == kRegular) {   // (JNIEnv*, jobject/jclass, ...)
    put(false, armtr_proxy_jnienv((void*)host_int()));
    put(false, host_int());
  }
  for (const char* p = args; *p && ns < 64; p++) {
    bool fp = *p == 'F' || *p == 'D';
    put(fp, norm(*p, fp ? host_fp() : host_int()));
  }
  uint64_t rx = 0, rv = 0;
  if (armtr_rt::call(cpu, t.guest, x, nx, v, nv, s, ns, &rx, &rv) < 0) guest_crash(cpu, "JNI method");
  char ret = t.shorty[0] ? t.shorty[0] : 'V';
  if (ret == 'F' || ret == 'D') return ret == 'F' ? (uint32_t)rv : rv;
  return ret == 'V' ? 0 : norm(ret, rx);
}

extern "C" {
// ART looks up this symbol in the library named by ro.dalvik.vm.native.bridge.
__attribute__((visibility("default"))) android::NativeBridgeCallbacks NativeBridgeItf = {
    kVersion,
    &nb_initialize,
    &nb_loadLibrary,
    &nb_getTrampoline,
    &nb_isSupported,
    &nb_getAppEnv,
    &nb_isCompatibleWith,
    &nb_getSignalHandler,
    &nb_unloadLibrary,
    &nb_getError,
    &nb_isPathSupported,
    &nb_initAnonymousNamespace,
    &nb_createNamespace,
    &nb_linkNamespaces,
    &nb_loadLibraryExt,
    &nb_getVendorNamespace,
    &nb_getExportedNamespace,
    &nb_preZygoteFork,
    &nb_getTrampolineWithJNICallType,
    &nb_getTrampolineForFunctionPointer,
    &nb_isNativeBridgeFunctionPointer,
};
}
