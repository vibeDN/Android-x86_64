// SPDX-License-Identifier: GPL-2.0-or-later
// armtr guest runtime, see guest_rt.h. Moved out of runner/armtr_run.cpp so the NativeBridge shares it.
#include "guest_rt.h"

#include <elf.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "armtr_loader_ext.h"
#include "host.h"

namespace armtr_rt {
namespace {

constexpr size_t kThreadHostStack = 2 << 20;
constexpr uint64_t kExeBase = 0x5500000000ull;     // like the kernel's ELF_ET_DYN_BASE for PIE
constexpr uint64_t kInterpBase = 0x7e00000000ull;  // ET_DYN interpreter
constexpr uint64_t kNeverAddr = 0x1000;            // stop address of armtr_run: guest code never gets there

std::string g_sysroot, g_self_exe;
bool g_trace = false;
armtr_syscall_intercept_fn g_hook;
armtr_cpu* g_main_cpu;
std::atomic<int> g_threads{0};
std::atomic<int> g_main_exit{0};

thread_local uint64_t t_clear_tid = 0;   // guest CLONE_CHILD_CLEARTID address of this thread
thread_local armtr_cpu* t_cpu = nullptr;
thread_local int t_depth = 0;
thread_local uint64_t t_top_sp = 0;
thread_local armtr_regs* t_capture = nullptr;
uint64_t g_linker_lo, g_linker_hi;       // guest linker64 image (its debuggerd handlers are not guest handlers)

// ---- safe guest memory access (EFAULT instead of a host crash) ----
long vm_copy(bool out, uint64_t guest, void* local, size_t n) {
  struct iovec { void* b; size_t l; } l{local, n}, r{(void*)guest, n};
  return rawsc(out ? SYS_process_vm_writev : SYS_process_vm_readv, getpid(), (long)&l, 1, (long)&r, 1, 0);
}
// Reads a NUL terminated guest string; false if unreadable or longer than max-1.
bool guest_str(uint64_t g, char* buf, size_t max) {
  long n = vm_copy(false, g, buf, max);
  if (n <= 0 && n != -EPERM && n != -ENOSYS) return false;
  if (n <= 0) { n = (long)strnlen((const char*)g, max); if (n == (long)max) return false; buf[n] = 0; return true; }
  return memchr(buf, 0, (size_t)n) != nullptr;
}

bool mapped_prefix(const char* p) {
  static const char* const pre[] = {"/system", "/apex", "/linkerconfig", "/vendor", "/product", "/system_ext",
                                    "/odm", "/data/local/tmp"};
  for (const char* x : pre) {
    size_t n = strlen(x);
    if (!strncmp(p, x, n) && (p[n] == '/' || p[n] == 0)) return true;
  }
  return false;
}

// Rewrites the path argument at index `pi`, then lets the normal syscall layer run with the new pointer.
int64_t with_path(armtr_cpu* cpu, uint64_t nr, const uint64_t a[6], int pi) {
  char buf[PATH_MAX];
  std::string host;
  if (!guest_str(a[pi], buf, sizeof buf) || !map_path(buf, &host)) return ARMTR_SYSCALL_PASS;
  uint64_t b[6];
  memcpy(b, a, sizeof b);
  b[pi] = (uint64_t)host.c_str();
  return armtr_syscall(cpu, nr, b, nullptr);
}


// ---- guest signal delivery ----
// Guest dispositions live in the syscalls module's table. A guest handler for an asynchronous signal gets a host
// handler (unless the host already handles that signal itself, e.g. ART/debuggerd in an app) that records the
// siginfo per guest thread and kicks the thread's cpu; at the next safe point the engine calls on_signal(), which
// builds the arm64 rt_sigframe on the guest stack. The guest's signal mask *is* the host thread's mask
// (rt_sigprocmask passes through), so the host kernel does the blocking. Synchronous faults come from the engine.
constexpr uint64_t kSA_ONSTACK = 0x08000000, kSA_RESTART = 0x10000000, kSA_NODEFER = 0x40000000,
                   kSA_RESETHAND = 0x80000000, kSA_RESTORER = 0x04000000;
constexpr size_t kSiginfo = 128, kUc = 128, kMctx = kUc + 176, kResv = kMctx + 288, kFrame = kResv + 4096;  // 4688
static_assert(kFrame % 16 == 0, "rt_sigframe size");
constexpr uint32_t kFpsimdMagic = 0x46508001, kFpsimdSize = 528, kEsrMagic = 0x45535201;

struct Pending { std::atomic<uint64_t> bits{0}; siginfo_t info[65]; };
thread_local Pending t_sig;   // recorded by the host handler on this thread
Pending g_proc_sig;           // arrived on a thread without guest cpu: first guest thread to pass a safe point
std::mutex g_act_mu;
struct sigaction g_host_old[65];
bool g_host_owned[65];
// `mov x8, #139; svc #0`: x30 for handlers installed without SA_RESTORER (bionic always passes __restore_rt).
alignas(16) const uint32_t kSigreturnTramp[2] = {0xD2801168u, 0xD4000001u};
struct GStack { uint64_t sp; int32_t flags, pad; uint64_t size; };

bool sync_sig(int s) { return s == SIGSEGV || s == SIGBUS || s == SIGILL || s == SIGTRAP || s == SIGFPE; }
bool linker_handler(uint64_t h) { return h >= g_linker_lo && h < g_linker_hi; }
uint64_t host_mask() { uint64_t m = 0; rawsc(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&m, 8); return m; }
void set_host_mask(uint64_t m) { rawsc(SYS_rt_sigprocmask, SIG_SETMASK, (long)&m, 0, 8); }

void host_handler(int sig, siginfo_t* si, void*) {   // async-signal-safe
  armtr_cpu* c = t_cpu;
  Pending& p = c ? t_sig : g_proc_sig;
  p.info[sig] = *si;
  p.bits.fetch_or(1ull << (sig - 1));
  if (c) armtr_kick(c);
}

// After the guest changed the disposition of `sig`: make the host deliver it to host_handler, or give it back.
void update_host_action(int sig, const armtr_guest_sigaction& nw) {
  if (sig < 1 || sig > 64 || sync_sig(sig) || sig == SIGKILL || sig == SIGSTOP) return;
  std::lock_guard<std::mutex> l(g_act_mu);
  if (nw.handler > 1 && !linker_handler(nw.handler)) {
    if (!g_host_owned[sig]) {
      struct sigaction cur;
      if (sigaction(sig, nullptr, &cur)) return;
      if (!(cur.sa_flags & SA_SIGINFO) && cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN) return;  // host's
      if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction) return;
      g_host_old[sig] = cur;
      g_host_owned[sig] = true;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = host_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;   // never SA_RESTART: the guest's own restart rule is applied in on_signal
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
  } else if (g_host_owned[sig]) {
    g_host_owned[sig] = false;
    if (nw.handler == 0) sigaction(sig, &g_host_old[sig], nullptr);   // SIG_IGN was mirrored by the syscalls module
  }
}

void set_guest_action(armtr_cpu* cpu, int sig, const armtr_guest_sigaction& a) {
  uint64_t k[4] = {a.handler, a.flags, a.restorer, a.mask}, args[6] = {(uint64_t)sig, (uint64_t)k, 0, 8, 0, 0};
  if (armtr_syscall(cpu, 134, args, nullptr) == 0) update_host_action(sig, a);
}

// Takes the lowest pending signal not blocked by `mask` (thread queue first).
int take_pending(uint64_t mask, siginfo_t* out) {
  uint64_t all = ~0ull, old;
  rawsc(SYS_rt_sigprocmask, SIG_SETMASK, (long)&all, (long)&old, 8);   // vs. host_handler on this thread
  int sig = 0;
  for (Pending* p : {&t_sig, &g_proc_sig}) {
    uint64_t b = p->bits.load() & ~mask;
    if (!b) continue;
    sig = __builtin_ctzll(b) + 1;
    *out = p->info[sig];
    p->bits.fetch_and(~(1ull << (sig - 1)));
    break;
  }
  set_host_mask(old);
  return sig;
}

// Syscalls the kernel restarts after a handler with SA_RESTART (-ERESTARTSYS paths); others return -EINTR.
bool restartable(int64_t nr) {
  switch (nr) {
    case 29: case 25: case 32: case 56: case 63: case 64: case 65: case 66: case 95: case 202: case 203:
    case 206: case 207: case 211: case 212: case 242: case 260:
      return true;   // ioctl fcntl flock openat read write readv writev waitid accept connect sendto recvfrom
    default:         // sendmsg recvmsg accept4 wait4
      return false;
  }
}

[[noreturn]] void bad_frame(armtr_cpu* cpu, const char* what, uint64_t sp) {
  fprintf(stderr, "armtr: %s: guest signal frame at 0x%llx not accessible: %s\n", what, (unsigned long long)sp,
          describe_fault(cpu).c_str());
  fflush(nullptr);
  _exit(139);
}

// Builds the arm64 rt_sigframe (arch/arm64/kernel/signal.c: siginfo, ucontext{sigcontext, fpsimd_context,
// [esr_context], end}) below the guest sp or on the alternate stack and enters the handler.
void push_frame(armtr_cpu* cpu, int sig, const void* info, const armtr_guest_sigaction& act, uint64_t fault_addr,
                uint64_t esr, bool restart, uint64_t orig_x0) {
  armtr_regs r;
  armtr_regs_get(cpu, &r);
  if (restart) { r.pc -= 4; r.x[0] = orig_x0; }
  GStack ss{};
  uint64_t qa[6] = {0, (uint64_t)&ss, 0, 0, 0, 0};
  armtr_syscall(cpu, 132, qa, nullptr);   // current sigaltstack (kept by the syscalls module)
  bool on_alt = ss.size && r.sp > ss.sp && r.sp - ss.sp <= ss.size;
  uint64_t sp = r.sp;
  if ((act.flags & kSA_ONSTACK) && !(ss.flags & 2) && ss.size && !on_alt) sp = ss.sp + ss.size;
  uint64_t rec = (sp - 16) & ~15ull;   // frame record {x29, x30} for unwinders
  sp = rec - kFrame;
  std::vector<uint8_t> f(kFrame, 0);
  uint8_t* b = f.data();
  auto put = [&](size_t off, const void* p, size_t n) { memcpy(b + off, p, n); };
  auto put64 = [&](size_t off, uint64_t v) { put(off, &v, 8); };
  auto put32 = [&](size_t off, uint32_t v) { put(off, &v, 4); };
  put(0, info, kSiginfo);
  put64(kUc + 16, ss.sp);                                        // uc_stack
  put32(kUc + 24, !ss.size ? 2 : on_alt ? 1 : 0);                // SS_DISABLE / SS_ONSTACK
  put64(kUc + 32, ss.size);
  put64(kUc + 40, host_mask());                                  // uc_sigmask
  put64(kMctx, (sig == SIGSEGV || sig == SIGBUS) ? fault_addr : 0);
  put(kMctx + 8, r.x, 31 * 8);
  put64(kMctx + 256, r.sp);
  put64(kMctx + 264, r.pc);
  put64(kMctx + 272, r.nzcv & 0xF0000000u);                      // pstate: EL0t + NZCV
  size_t o = kResv;
  put32(o, kFpsimdMagic); put32(o + 4, kFpsimdSize); put32(o + 8, r.fpsr); put32(o + 12, r.fpcr);
  put(o + 16, r.v, 512);
  o += kFpsimdSize;
  if (esr) { put32(o, kEsrMagic); put32(o + 4, 16); put64(o + 8, esr); o += 16; }
  // end marker: already zero
  uint64_t fr[2] = {r.x[29], r.x[30]};
  if (vm_copy(true, sp, b, kFrame) != (long)kFrame || vm_copy(true, rec, fr, 16) != 16) bad_frame(cpu, "signal", sp);
  r.x[0] = (uint64_t)sig; r.x[1] = sp; r.x[2] = sp + kUc;
  r.sp = sp; r.x[29] = rec;
  r.x[30] = (act.flags & kSA_RESTORER) && act.restorer ? act.restorer : (uint64_t)kSigreturnTramp;
  r.pc = act.handler;
  armtr_regs_set(cpu, &r);
  set_host_mask(host_mask() | act.mask | ((act.flags & kSA_NODEFER) ? 0 : 1ull << (sig - 1)));
  if (act.flags & kSA_RESETHAND) set_guest_action(cpu, sig, armtr_guest_sigaction{0, 0, 0, 0});
}

int64_t do_sigreturn(armtr_cpu* cpu) {
  armtr_regs r;
  armtr_regs_get(cpu, &r);
  std::vector<uint8_t> f(kFrame);
  uint8_t* b = f.data();
  if (vm_copy(false, r.sp, b, kFrame) != (long)kFrame) bad_frame(cpu, "rt_sigreturn", r.sp);
  auto get64 = [&](size_t off) { uint64_t v; memcpy(&v, b + off, 8); return v; };
  auto get32 = [&](size_t off) { uint32_t v; memcpy(&v, b + off, 4); return v; };
  memcpy(r.x, b + kMctx + 8, 31 * 8);
  r.sp = get64(kMctx + 256);
  r.pc = get64(kMctx + 264);
  r.nzcv = get64(kMctx + 272) & 0xF0000000u;
  for (size_t o = kResv; o + 8 <= kFrame;) {   // records; extra_context (SVE/ZA spill) is never produced by us
    uint32_t magic = get32(o), size = get32(o + 4);
    if (!magic || size < 8 || o + size > kFrame) break;
    if (magic == kFpsimdMagic && size >= kFpsimdSize) { r.fpsr = get32(o + 8); r.fpcr = get32(o + 12); memcpy(r.v, b + o + 16, 512); }
    o += size;
  }
  armtr_regs_set(cpu, &r);
  set_host_mask(get64(kUc + 40));   // may let a pending host signal in: it is delivered after this svc
  return (int64_t)r.x[0];           // the engine writes the syscall result to x0
}

// Engine signal hook (see armtr_signal_fn).
int on_signal(armtr_cpu* cpu, const armtr_fault* ft, int64_t svc_nr, uint64_t svc_x0) {
  armtr_guest_sigaction act;
  if (ft) {   // kernel force_sig_fault: a blocked or default/ignored synchronous signal kills the process
    if (armtr_syscalls_get_sigaction(ft->signo, &act) || act.handler <= 1 || linker_handler(act.handler)) return 0;
    if (host_mask() & (1ull << (ft->signo - 1))) return 0;
    uint8_t si[kSiginfo] = {0};
    int32_t h[3] = {ft->signo, 0, ft->code};
    memcpy(si, h, 12);
    memcpy(si + 16, &ft->addr, 8);   // si_addr
    push_frame(cpu, ft->signo, si, act, ft->addr, ft->esr, false, 0);
    return 1;
  }
  bool first = true;
  siginfo_t si;
  while (int sig = take_pending(host_mask(), &si)) {
    if (armtr_syscalls_get_sigaction(sig, &act) || act.handler == 1) continue;
    if (act.handler == 0) {   // reset to SIG_DFL after the host signal arrived: default action now
      if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT) continue;
      signal(sig, SIG_DFL);
      raise(sig);
      continue;
    }
    bool restart = first && svc_nr >= 0 && (act.flags & kSA_RESTART) && restartable(svc_nr) &&
                   [&] { armtr_regs r; armtr_regs_get(cpu, &r); return (int64_t)r.x[0] == -EINTR; }();
    push_frame(cpu, sig, &si, act, 0, 0, restart, svc_x0);
    first = false;
  }
  return 0;
}

int64_t intercept(armtr_cpu* cpu, uint64_t nr, const uint64_t a[6], void*) {
  // Re-arm delivery of queued signals (unblocked by this rt_sigprocmask/rt_sigreturn, or process-wide ones).
  if (cpu && (t_sig.bits.load(std::memory_order_relaxed) || g_proc_sig.bits.load(std::memory_order_relaxed)))
    armtr_kick(cpu);
  if (g_trace) {
    const char* n = armtr_syscall_name(nr);
    fprintf(stderr, "[%ld sys %s(%llu) %llx %llx %llx %llx]\n", rawsc(SYS_gettid), n ? n : "?", (unsigned long long)nr,
            (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
  }
  if (g_hook) {
    int64_t r = g_hook(cpu, nr, a, nullptr);
    if (r != ARMTR_SYSCALL_PASS) return r;
  }
  switch (nr) {
    case 94:   // exit_group: guest memory/fds are the host process's, so just end the process
      fflush(nullptr);
      _exit((int)(a[0] & 0xff));
    case 93:   // exit: this guest thread only
      if (cpu == g_main_cpu) g_main_exit = (int)a[0];
      armtr_stop(cpu);
      return 0;
    case 134: {   // rt_sigaction: recorded by the syscalls module, the host side is ours
      int64_t r = armtr_syscall(cpu, nr, a, nullptr);
      armtr_guest_sigaction nw;
      if (r == 0 && a[1] && !armtr_syscalls_get_sigaction((int)a[0], &nw)) update_host_action((int)a[0], nw);
      return r;
    }
    case 139: return do_sigreturn(cpu);   // rt_sigreturn
    case 96:   // set_tid_address: the host libc owns the host thread's clear_child_tid; emulate it ourselves
      t_clear_tid = a[0];
      return rawsc(SYS_gettid);
    case 56: {                                                       // openat
      char buf[32];
      if (!g_self_exe.empty() && guest_str(a[1], buf, sizeof buf) && !strcmp(buf, "/proc/self/exe")) {
        std::string host;
        if (!map_path(g_self_exe.c_str(), &host)) host = g_self_exe;
        uint64_t b[6]; memcpy(b, a, sizeof b); b[1] = (uint64_t)host.c_str();
        return armtr_syscall(cpu, nr, b, nullptr);
      }
      return with_path(cpu, nr, a, 1);
    }
    case 48: case 439: return with_path(cpu, nr, a, 1);             // faccessat, faccessat2
    case 79: case 291: return with_path(cpu, nr, a, 1);             // newfstatat, statx
    case 43: return with_path(cpu, nr, a, 0);                       // statfs
    case 78: {                                                       // readlinkat
      char buf[PATH_MAX];
      if (!g_self_exe.empty() && guest_str(a[1], buf, sizeof buf) && !strcmp(buf, "/proc/self/exe")) {
        size_t n = std::min<size_t>(g_self_exe.size(), a[3]);
        if (vm_copy(true, a[2], (void*)g_self_exe.data(), n) != (long)n) return -EFAULT;
        return (int64_t)n;
      }
      int64_t r = with_path(cpu, nr, a, 1);
      if (r == ARMTR_SYSCALL_PASS) r = armtr_syscall(cpu, nr, a, nullptr);
      // Results inside the sysroot are reported as guest paths (e.g. readlink("/proc/self/fd/N")).
      size_t sl = g_sysroot.size();
      if (r > (int64_t)sl && sl && !memcmp((const char*)a[2], g_sysroot.data(), sl) && ((const char*)a[2])[sl] == '/') {
        memmove((char*)a[2], (const char*)a[2] + sl, (size_t)r - sl);
        r -= (int64_t)sl;
      }
      return r;
    }
    default: return ARMTR_SYSCALL_PASS;
  }
}

// ---- guest threads (clone with CLONE_THREAD) ----
struct ThreadArgs {
  armtr_regs regs;
  uint64_t flags, ctid, ptid;
  std::atomic<int> tid{0};
};

void* thread_main(void* p) {
  ThreadArgs* ta = (ThreadArgs*)p;
  int tid = (int)rawsc(SYS_gettid);
  armtr_cpu* cpu = armtr_cpu_create(armtr_syscall, nullptr);
  armtr_regs r = ta->regs;
  uint64_t flags = ta->flags, ctid = ta->ctid, ptid = ta->ptid;
  if (flags & 0x00100000) vm_copy(true, ptid, &tid, 4);   // CLONE_PARENT_SETTID
  if (flags & 0x01000000) vm_copy(true, ctid, &tid, 4);   // CLONE_CHILD_SETTID
  if (flags & 0x00200000) t_clear_tid = ctid;             // CLONE_CHILD_CLEARTID
  ta->tid.store(cpu ? tid : -ENOMEM);
  rawsc(SYS_futex, (long)&ta->tid, FUTEX_WAKE_PRIVATE, 1);   // parent waits for the tid; ta dies after this
  ta = nullptr;
  if (!cpu) { g_threads--; return nullptr; }
  t_cpu = cpu;
  armtr_regs_set(cpu, &r);
  t_depth++;
  int rc = armtr_run(cpu, kNeverAddr);
  t_depth--;
  if (rc < 0) {
    fprintf(stderr, "armtr: %s\n", describe_fault(cpu).c_str());
    fflush(nullptr);
    _exit(139);
  }
  t_cpu = nullptr;
  armtr_cpu_destroy(cpu);
  if (t_clear_tid) {
    int zero = 0;
    vm_copy(true, t_clear_tid, &zero, 4);   // fails silently if the guest already unmapped it, like the kernel
    rawsc(SYS_futex, (long)t_clear_tid, FUTEX_WAKE, INT_MAX);
    rawsc(SYS_futex, (long)t_clear_tid, FUTEX_WAKE_PRIVATE, INT_MAX);
  }
  g_threads--;
  return nullptr;
}

int thread_hook(const armtr_regs* regs, uint64_t flags, uint64_t ctid, uint64_t ptid, void*) {
  if (t_capture) {   // capture_next_clone(): the "child" is this very host thread
    *t_capture = *regs;
    t_capture = nullptr;
    int tid = (int)rawsc(SYS_gettid);
    if (flags & 0x00100000) vm_copy(true, ptid, &tid, 4);
    if (flags & 0x01000000) vm_copy(true, ctid, &tid, 4);
    return tid;
  }
  auto* ta = new ThreadArgs;   // freed below once the child published its tid
  ta->regs = *regs; ta->flags = flags; ta->ctid = ctid; ta->ptid = ptid;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&at, kThreadHostStack);
  pthread_t th;
  g_threads++;
  int rc = pthread_create(&th, &at, thread_main, ta);
  pthread_attr_destroy(&at);
  if (rc) { g_threads--; delete ta; return -rc; }
  int tid;
  while ((tid = ta->tid.load()) == 0) rawsc(SYS_futex, (long)&ta->tid, FUTEX_WAIT_PRIVATE, 0, 0);
  // ponytail: ThreadArgs is leaked (~800 bytes/thread): the child still touches it after the wake, no cheap owner
  return tid;
}

uint64_t id_reg(armtr_cpu* cpu, uint32_t mrs_insn) {
  static uint32_t* code = nullptr;
  static int used = 0;
  if (!code) {
    code = (uint32_t*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    armtr_mem_map((uint64_t)code, 4096, 7);
  }
  uint32_t* f = code + 2 * used++;
  f[0] = mrs_insn; f[1] = 0xD65F03C0;   // mrs x0, <reg> ; ret
  armtr_mem_map((uint64_t)f, 8, 7);
  uint64_t ret = 0;
  if (call(cpu, (uint64_t)f, nullptr, 0, nullptr, 0, nullptr, 0, &ret) < 0) {
    fprintf(stderr, "armtr: cpu probe failed: %s\n", armtr_last_error(cpu));
    abort();
  }
  if (used >= 2048) used = 0;
  return ret;
}
unsigned fld(uint64_t v, int lo) { return (unsigned)((v >> lo) & 0xf); }

std::string read_interp(const std::string& path, std::string* err) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { *err = path + ": " + strerror(errno); return ""; }
  Elf64_Ehdr eh;
  std::string interp;
  if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, 4) || eh.e_machine != EM_AARCH64) {
    *err = path + ": not an aarch64 ELF";
    fclose(f);
    return "";
  }
  for (int i = 0; i < eh.e_phnum; i++) {
    Elf64_Phdr ph;
    fseek(f, (long)(eh.e_phoff + (uint64_t)i * eh.e_phentsize), SEEK_SET);
    if (fread(&ph, sizeof ph, 1, f) != 1) break;
    if (ph.p_type == PT_INTERP) {
      std::vector<char> b(ph.p_filesz + 1, 0);
      fseek(f, (long)ph.p_offset, SEEK_SET);
      if (fread(b.data(), 1, ph.p_filesz, f) == ph.p_filesz) interp = b.data();
    }
  }
  fclose(f);
  return interp;
}

}  // namespace

void set_sysroot(const std::string& dir) {
  g_sysroot = dir;
  while (g_sysroot.size() > 1 && g_sysroot.back() == '/') g_sysroot.pop_back();
}
bool map_path(const char* in, std::string* out) {
  if (g_sysroot.empty() || in[0] != '/' || !mapped_prefix(in)) return false;
  *out = g_sysroot + in;
  return true;
}
void set_trace(bool on) { g_trace = on; }
void set_proc_self_exe(const std::string& p) { g_self_exe = p; }
void set_embedder_hook(armtr_syscall_intercept_fn fn) { g_hook = fn; }

int init() {
  static std::once_flag once;
  static int rc = -1;
  std::call_once(once, [] {
    rc = armtr_init();
    armtr_syscalls_set_intercept(intercept, nullptr);
    armtr_syscalls_set_thread_hook(thread_hook, nullptr);
    armtr_set_signal_hook(on_signal);
    armtr_mem_map((uint64_t)kSigreturnTramp, sizeof kSigreturnTramp, 5);
  });
  return rc;
}

armtr_cpu* thread_cpu() { return t_cpu; }
void set_thread_cpu(armtr_cpu* c) { t_cpu = c; }
int& run_depth() { return t_depth; }
void set_call_stack_top(uint64_t sp) { t_top_sp = sp & ~15ull; }
void capture_next_clone(armtr_regs* out) { t_capture = out; }
void set_main_cpu(armtr_cpu* c) { g_main_cpu = c; }
int main_exit_code() { return g_main_exit; }
int live_threads() { return g_threads.load(); }

// The engine's CPU model (UC_CPU_ARM64_MAX) is asked what it implements by executing `mrs` of the ID
// registers in the guest; the auxv bits are derived from the answer, so they cannot claim anything Unicorn
// would then fault on. SVE, pointer auth, MTE, BTI, RNG, TME are deliberately never advertised: bionic
// would switch to code paths (tagging, PAC keys via prctl) that the syscall layer does not support.
void probe_hwcap(armtr_cpu* cpu, uint64_t* hw1, uint64_t* hw2, bool show) {
  uint64_t isar0 = id_reg(cpu, 0xD5380600), isar1 = id_reg(cpu, 0xD5380620), pfr0 = id_reg(cpu, 0xD5380400),
           mmfr2 = id_reg(cpu, 0xD5380740);
  uint64_t h = 0, h2 = 0;
  auto set = [&](bool c, int bit) { if (c) h |= 1ull << bit; };
  auto set2 = [&](bool c, int bit) { if (c) h2 |= 1ull << bit; };
  bool fp = fld(pfr0, 16) != 0xf, asimd = fld(pfr0, 20) != 0xf;
  set(fp, 0); set(asimd, 1);
  set(fld(isar0, 4) >= 1, 3);  set(fld(isar0, 4) >= 2, 4);     // AES, PMULL
  set(fld(isar0, 8) >= 1, 5);  set(fld(isar0, 12) >= 1, 6);    // SHA1, SHA2
  set(fld(isar0, 16) >= 1, 7); set(fld(isar0, 20) >= 2, 8);    // CRC32, ATOMICS (LSE)
  set(fp && fld(pfr0, 16) == 1, 9); set(asimd && fld(pfr0, 20) == 1, 10);   // FPHP, ASIMDHP
  set(true, 11);                                                // CPUID (mrs of ID regs works, see above)
  set(fld(isar0, 28) >= 1, 12);                                 // ASIMDRDM
  set(fld(isar1, 12) >= 1, 13); set(fld(isar1, 16) >= 1, 14);   // JSCVT, FCMA
  set(fld(isar1, 20) >= 1, 15); set(fld(isar1, 20) >= 2, 26);   // LRCPC, ILRCPC
  set(fld(isar1, 0) >= 1, 16);                                  // DCPOP
  set(fld(isar0, 32) >= 1, 17); set(fld(isar0, 36) >= 1, 18); set(fld(isar0, 40) >= 1, 19);   // SHA3 SM3 SM4
  set(fld(isar0, 44) >= 1, 20); set(fld(isar0, 12) >= 2, 21);   // ASIMDDP, SHA512
  set(fld(isar0, 48) >= 1, 23);                                 // ASIMDFHM
  set(fld(pfr0, 48) >= 1, 24);                                  // DIT
  set(fld(mmfr2, 32) >= 1, 25);                                 // USCAT
  set(fld(isar0, 52) >= 1, 27);                                 // FLAGM
  set2(fld(isar1, 0) >= 2, 0);                                  // DCPODP
  set2(fld(isar0, 52) >= 2, 7);                                 // FLAGM2
  set2(fld(isar1, 32) >= 1, 8);                                 // FRINT
  set2(fld(isar1, 44) >= 1, 14);                                // BF16
  set2(fld(isar1, 52) >= 1, 13);                                // I8MM
  *hw1 = h; *hw2 = h2;
  if (show)
    fprintf(stderr, "armtr: cpu model UC_CPU_ARM64_MAX isar0=%016llx isar1=%016llx pfr0=%016llx mmfr2=%016llx\n"
            "armtr: AT_HWCAP=0x%llx AT_HWCAP2=0x%llx (SVE/PAC/MTE/BTI/RNG masked)\n", (unsigned long long)isar0,
            (unsigned long long)isar1, (unsigned long long)pfr0, (unsigned long long)mmfr2, (unsigned long long)h,
            (unsigned long long)h2);
}

bool setup_exec(armtr_cpu* cpu, const std::string& exe_path, const std::string& exe_guest,
                const std::vector<std::string>& gargs, const std::vector<std::string>& env, Exec* out,
                std::string* err) {
  uint64_t hw1, hw2;
  probe_hwcap(cpu, &hw1, &hw2, false);
  char e[512];
  std::string interp = read_interp(exe_path, err);
  if (!err->empty()) return false;
  armtr_image* exe = armtr_load_raw(exe_path.c_str(), kExeBase, e, sizeof e);
  if (!exe) { *err = "load " + exe_path + ": " + e; return false; }
  armtr_image* lin = nullptr;
  if (!interp.empty()) {
    std::string host;
    if (!map_path(interp.c_str(), &host)) host = interp;
    lin = armtr_load_raw(host.c_str(), kInterpBase, e, sizeof e);
    if (!lin) lin = armtr_load_raw(host.c_str(), 0, e, sizeof e);
    if (!lin) { *err = "load interpreter " + host + ": " + e; return false; }
    // Its address range: bionic's linker installs debuggerd signal handlers, which armtr does not run (a guest
    // crash is reported by armtr / the host's debuggerd instead).
    g_linker_lo = armtr_image_base(lin);
    g_linker_hi = g_linker_lo + armtr_image_size(lin);
  }
  uint64_t phdr = 0, phnum = 0, phent = 0;
  if (armtr_image_phdr(exe, &phdr, &phnum, &phent)) { *err = exe_path + ": program headers not mapped"; return false; }

  // Initial stack: strings, then (from sp up) argc, argv[], 0, envp[], 0, auxv[]
  uint64_t sbase; size_t ssize;
  armtr_cpu_stack(cpu, &sbase, &ssize);
  uint64_t top = sbase + ssize;
  auto push_bytes = [&](const void* p, size_t n) { top -= n; memcpy((void*)top, p, n); return top; };
  auto push_str = [&](const std::string& s) { return push_bytes(s.c_str(), s.size() + 1); };
  uint64_t execfn = push_str(exe_guest);
  std::vector<uint64_t> envp, argp;
  for (size_t k = env.size(); k-- > 0;) envp.insert(envp.begin(), push_str(env[k]));
  for (size_t k = gargs.size(); k-- > 0;) argp.insert(argp.begin(), push_str(gargs[k]));
  uint64_t platform = push_str("aarch64");
  uint8_t rnd[16];
  if (getrandom(rnd, sizeof rnd, 0) != 16) memset(rnd, 0x5a, sizeof rnd);
  uint64_t random = push_bytes(rnd, 16);
  std::vector<uint64_t> av;
  auto add = [&](uint64_t t, uint64_t x) { av.push_back(t); av.push_back(x); };
  add(AT_PHDR, phdr); add(AT_PHENT, phent); add(AT_PHNUM, phnum); add(AT_PAGESZ, 4096);
  add(AT_BASE, lin ? armtr_image_base(lin) : 0); add(AT_FLAGS, 0); add(AT_ENTRY, armtr_image_entry(exe));
  add(AT_UID, getuid()); add(AT_EUID, geteuid()); add(AT_GID, getgid()); add(AT_EGID, getegid());
  add(AT_PLATFORM, platform); add(AT_HWCAP, hw1); add(AT_CLKTCK, 100); add(AT_SECURE, 0);
  add(AT_RANDOM, random); add(AT_HWCAP2, hw2); add(AT_EXECFN, execfn);
  add(51 /*AT_MINSIGSTKSZ*/, 5120);
  add(AT_NULL, 0);
  size_t words = 1 + argp.size() + 1 + envp.size() + 1 + av.size();
  top &= ~15ull;
  uint64_t sp = (top - words * 8) & ~15ull;
  uint64_t* w = (uint64_t*)sp;
  *w++ = argp.size();
  for (auto p : argp) *w++ = p;
  *w++ = 0;
  for (auto p : envp) *w++ = p;
  *w++ = 0;
  for (auto x : av) *w++ = x;

  armtr_regs r;
  memset(&r, 0, sizeof r);
  r.sp = sp;
  r.pc = armtr_image_entry(lin ? lin : exe);
  armtr_regs_set(cpu, &r);
  out->exe = exe; out->interp = lin; out->entry = r.pc; out->sp = sp;
  return true;
}

std::string describe_fault(armtr_cpu* cpu) {
  armtr_regs r; armtr_regs_get(cpu, &r);
  uint64_t fpc = 0;
  armtr_fault_info(cpu, nullptr, &fpc);
  if (!fpc) fpc = r.pc;
  char b[1600];
  int n = snprintf(b, sizeof b, "guest fault: %s (pc=0x%llx lr=0x%llx sp=0x%llx)", armtr_last_error(cpu),
                   (unsigned long long)r.pc, (unsigned long long)r.x[30], (unsigned long long)r.sp);
  // Which file/offset is the faulting pc in? (guest file mappings are real host mappings)
  if (FILE* m = fopen("/proc/self/maps", "r")) {
    char line[1024];
    while (fgets(line, sizeof line, m)) {
      unsigned long lo, hi, off;
      if (sscanf(line, "%lx-%lx %*s %lx", &lo, &hi, &off) == 3 && fpc >= lo && fpc < hi) {
        line[strcspn(line, "\n")] = 0;
        snprintf(b + n, sizeof b - n, "  pc 0x%llx in %s (file offset 0x%lx)", (unsigned long long)fpc, line,
                 off + (fpc - lo));
        break;
      }
    }
    fclose(m);
  }
  return b;
}

int call(armtr_cpu* cpu, uint64_t entry, const uint64_t* x, int nx, const uint64_t* v, int nv,
         const uint64_t* stk, int ns, uint64_t* x0, uint64_t* v0) {
  if (nx > 8 || nv > 8 || ns > 64) return -1;
  armtr_regs saved; armtr_regs_get(cpu, &saved);
  armtr_regs r = saved;
  uint64_t sbase; size_t ssize;
  armtr_cpu_stack(cpu, &sbase, &ssize);
  // Nested (a host callback is running inside guest code on this cpu): below the live guest frame. AArch64 has
  // no red zone; 512 bytes keep a margin. Outermost: the cpu's own stack.
  uint64_t sp = t_depth > 0 ? ((saved.sp - 512) & ~15ull) : (t_top_sp ? t_top_sp : sbase + ssize);
  sp = (sp - (uint64_t)ns * 8) & ~15ull;
  if (ns) memcpy((void*)sp, stk, (size_t)ns * 8);
  for (int i = 0; i < nx; i++) r.x[i] = x[i];
  for (int i = 0; i < nv; i++) { memset(r.v[i], 0, 16); memcpy(r.v[i], &v[i], 8); }
  uint64_t stop = sbase - 4096;   // the stack's guard page: never fetched (exit check precedes fetch)
  r.sp = sp; r.x[30] = stop; r.pc = entry;
  armtr_regs_set(cpu, &r);
  t_depth++;
  int rc = armtr_run(cpu, stop);
  t_depth--;
  if (rc == 0) {
    armtr_regs o; armtr_regs_get(cpu, &o);
    if (x0) *x0 = o.x[0];
    if (v0) memcpy(v0, o.v[0], 8);
  }
  // Callee-saved state, tpidr_el0 and the outer frame are restored; a nested failure is the caller's to report.
  if (rc == 0 || t_depth > 0) armtr_regs_set(cpu, &saved);
  return rc;
}

}  // namespace armtr_rt
