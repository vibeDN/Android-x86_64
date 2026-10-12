// SPDX-License-Identifier: GPL-2.0-or-later
// armtr-run: run an aarch64 Linux/Bionic program (static, or dynamic with its guest linker) on x86_64
// through the armtr modules (engine + loader + syscalls). See docs/translator/04-runner.md.
//
// Usage: armtr-run [--sysroot DIR] [--host-env] [-E K=V]... [--show-cpu] prog [args...]
//   ARMTR_TRACE=1 prints every guest syscall to stderr.
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <stdarg.h>
#include <string>
#include <vector>

#include "armtr/engine.h"
#include "armtr/loader.h"
#include "armtr_loader_ext.h"
#include "armtr_syscalls_ext.h"
#include "host.h"

extern char** environ;

namespace {

constexpr size_t kMainStack = 8 << 20;
constexpr size_t kThreadHostStack = 2 << 20;
constexpr uint64_t kExeBase = 0x5500000000ull;     // like the kernel's ELF_ET_DYN_BASE for PIE
constexpr uint64_t kInterpBase = 0x7e00000000ull;  // ET_DYN interpreter
constexpr uint64_t kNeverAddr = 0x1000;            // stop address of armtr_run: guest code never gets there

std::string g_sysroot;
bool g_trace = false;
std::string g_exe_real;                // absolute path of the guest executable (for /proc/self/exe)
armtr_cpu* g_main_cpu;
std::atomic<int> g_threads{0};         // live non-main guest threads
std::atomic<int> g_main_exit{0};

thread_local uint64_t t_clear_tid = 0;   // guest CLONE_CHILD_CLEARTID address of this thread
thread_local int t_exit_code = 0;

[[noreturn]] void die(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void die(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "armtr-run: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
  va_end(ap);
  _exit(127);
}

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

// ---- path mapping: Android absolute paths -> sysroot ----
bool mapped_prefix(const char* p) {
  static const char* const pre[] = {"/system", "/apex", "/linkerconfig", "/vendor", "/product", "/system_ext",
                                    "/odm", "/data/local/tmp"};
  for (const char* x : pre) {
    size_t n = strlen(x);
    if (!strncmp(p, x, n) && (p[n] == '/' || p[n] == 0)) return true;
  }
  return false;
}
// Fills out with the host path for guest path `in` (returns false: pass the original through).
bool map_path(const char* in, std::string* out) {
  if (g_sysroot.empty() || in[0] != '/' || !mapped_prefix(in)) return false;
  *out = g_sysroot + in;
  return true;
}

// ---- intercept ----
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

int64_t intercept(armtr_cpu* cpu, uint64_t nr, const uint64_t a[6], void*) {
  if (g_trace) {
    const char* n = armtr_syscall_name(nr);
    fprintf(stderr, "[%ld sys %s(%llu) %llx %llx %llx %llx]\n", rawsc(SYS_gettid), n ? n : "?", (unsigned long long)nr,
            (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3]);
    if (nr == 98) {   // futex: also show the result and the word
      int64_t r = armtr_syscall(cpu, nr, a, nullptr);
      fprintf(stderr, "[%ld   -> %lld word=%x]\n", rawsc(SYS_gettid), (long long)r, *(unsigned*)a[0]);
      return r;
    }
  }
  switch (nr) {
    case 94:   // exit_group: guest memory/fds are the host process's, so just end the process
      fflush(nullptr);
      _exit((int)(a[0] & 0xff));
    case 93:   // exit: this guest thread only
      t_exit_code = (int)a[0];
      if (cpu == g_main_cpu) g_main_exit = (int)a[0];
      armtr_stop(cpu);
      return 0;
    case 96:   // set_tid_address: the host libc owns the host thread's clear_child_tid; emulate it ourselves
      t_clear_tid = a[0];
      return rawsc(SYS_gettid);
    case 56: return with_path(cpu, nr, a, 1);                       // openat
    case 48: case 439: return with_path(cpu, nr, a, 1);             // faccessat, faccessat2
    case 79: case 291: return with_path(cpu, nr, a, 1);             // newfstatat, statx
    case 43: return with_path(cpu, nr, a, 0);                       // statfs
    case 78: {                                                       // readlinkat
      char buf[PATH_MAX];
      if (guest_str(a[1], buf, sizeof buf) && !strcmp(buf, "/proc/self/exe")) {
        size_t n = std::min<size_t>(g_exe_real.size(), a[3]);
        if (vm_copy(true, a[2], (void*)g_exe_real.data(), n) != (long)n) return -EFAULT;
        return (int64_t)n;
      }
      return with_path(cpu, nr, a, 1);
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

void report_fault(armtr_cpu* cpu) {
  armtr_regs r; armtr_regs_get(cpu, &r);
  fprintf(stderr, "armtr-run: guest fault: %s (pc=0x%llx lr=0x%llx sp=0x%llx)\n", armtr_last_error(cpu),
          (unsigned long long)r.pc, (unsigned long long)r.x[30], (unsigned long long)r.sp);
  // Which file/offset is pc in? (guest file mappings are real host mappings)
  if (FILE* m = fopen("/proc/self/maps", "r")) {
    char line[1024];
    while (fgets(line, sizeof line, m)) {
      unsigned long lo, hi, off;
      if (sscanf(line, "%lx-%lx %*s %lx", &lo, &hi, &off) == 3 && r.pc >= lo && r.pc < hi) {
        fprintf(stderr, "armtr-run:   pc in %s  (file offset 0x%lx)", line, off + (r.pc - lo));
        break;
      }
    }
    fclose(m);
  }
  fflush(nullptr);
  _exit(139);
}

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
  armtr_regs_set(cpu, &r);
  if (armtr_run(cpu, kNeverAddr) < 0) report_fault(cpu);
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

// ---- CPU feature probe -> AT_HWCAP/AT_HWCAP2 ----
// The engine's CPU model (UC_CPU_ARM64_MAX) is asked what it implements by executing `mrs` of the ID
// registers in the guest; the auxv bits are derived from the answer, so they cannot claim anything Unicorn
// would then fault on. SVE, pointer auth, MTE, BTI, RNG, TME are deliberately never advertised: bionic
// would switch to code paths (tagging, PAC keys via prctl) that the syscall layer does not support.
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
  if (armtr_call(cpu, (uint64_t)f, nullptr, 0, &ret) < 0) die("cpu probe failed: %s", armtr_last_error(cpu));
  if (used >= 2048) used = 0;
  return ret;
}
unsigned fld(uint64_t v, int lo) { return (unsigned)((v >> lo) & 0xf); }

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
    fprintf(stderr, "armtr-run: cpu model UC_CPU_ARM64_MAX isar0=%016llx isar1=%016llx pfr0=%016llx mmfr2=%016llx\n"
            "armtr-run: AT_HWCAP=0x%llx AT_HWCAP2=0x%llx (SVE/PAC/MTE/BTI/RNG masked)\n", (unsigned long long)isar0,
            (unsigned long long)isar1, (unsigned long long)pfr0, (unsigned long long)mmfr2, (unsigned long long)h,
            (unsigned long long)h2);
}

// ---- process image setup ----
std::string read_interp(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) die("cannot open %s: %s", path.c_str(), strerror(errno));
  Elf64_Ehdr eh;
  std::string interp;
  if (fread(&eh, sizeof eh, 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, 4)) die("%s: not an ELF file", path.c_str());
  if (eh.e_machine != EM_AARCH64) die("%s: not an aarch64 ELF", path.c_str());
  for (int i = 0; i < eh.e_phnum; i++) {
    Elf64_Phdr ph;
    fseek(f, (long)(eh.e_phoff + (uint64_t)i * eh.e_phentsize), SEEK_SET);
    if (fread(&ph, sizeof ph, 1, f) != 1) break;
    if (ph.p_type == PT_INTERP) {
      std::vector<char> b(ph.p_filesz + 1, 0);
      fseek(f, (long)ph.p_offset, SEEK_SET);
      if (fread(b.data(), 1, ph.p_filesz, f) != ph.p_filesz) die("%s: bad PT_INTERP", path.c_str());
      interp = b.data();
    }
  }
  fclose(f);
  return interp;
}

struct Auxv { std::vector<uint64_t> v; void add(uint64_t t, uint64_t x) { v.push_back(t); v.push_back(x); } };

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> env = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data", "PATH=/system/bin"};
  bool show_cpu = false;
  g_sysroot = getenv("ARMTR_SYSROOT") ? getenv("ARMTR_SYSROOT") : "";
  g_trace = getenv("ARMTR_TRACE") != nullptr;
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    std::string a = argv[i];
    if (a == "--sysroot" && i + 1 < argc) g_sysroot = argv[++i];
    else if (a == "--host-env") { env.clear(); for (char** e = environ; *e; e++) env.push_back(*e); }
    else if (a == "-E" && i + 1 < argc) env.push_back(argv[++i]);
    else if (a == "--show-cpu") show_cpu = true;
    else if (a == "--") { i++; break; }
    else die("unknown option %s\nusage: armtr-run [--sysroot DIR] [--host-env] [-E K=V] [--show-cpu] prog [args]", a.c_str());
  }
  if (i >= argc) die("usage: armtr-run [--sysroot DIR] [--host-env] [-E K=V] [--show-cpu] prog [args]");
  while (g_sysroot.size() > 1 && g_sysroot.back() == '/') g_sysroot.pop_back();
  char rp[PATH_MAX];
  if (!realpath(argv[i], rp)) die("%s: %s", argv[i], strerror(errno));
  g_exe_real = rp;
  std::vector<std::string> gargs;
  gargs.push_back(argv[i]);
  for (int k = i + 1; k < argc; k++) gargs.push_back(argv[k]);

  if (armtr_init()) die("armtr_init failed");
  armtr_syscalls_set_intercept(intercept, nullptr);
  armtr_syscalls_set_thread_hook(thread_hook, nullptr);
  g_main_cpu = armtr_cpu_create_ex(armtr_syscall, nullptr, kMainStack);
  if (!g_main_cpu) die("cannot create cpu");
  uint64_t hw1, hw2;
  probe_hwcap(g_main_cpu, &hw1, &hw2, show_cpu);

  // Map exe (and its interpreter) the way execve does: segments only, no relocation, no DT_NEEDED.
  char err[512];
  std::string interp = read_interp(g_exe_real);
  armtr_image* exe = armtr_load_raw(g_exe_real.c_str(), kExeBase, err, sizeof err);
  if (!exe) die("load %s: %s", g_exe_real.c_str(), err);
  armtr_image* lin = nullptr;
  if (!interp.empty()) {
    std::string host;
    if (!map_path(interp.c_str(), &host)) host = interp;
    lin = armtr_load_raw(host.c_str(), kInterpBase, err, sizeof err);
    if (!lin) lin = armtr_load_raw(host.c_str(), 0, err, sizeof err);
    if (!lin) die("load interpreter %s: %s", host.c_str(), err);
  }
  uint64_t phdr = 0, phnum = 0, phent = 0;
  if (armtr_image_phdr(exe, &phdr, &phnum, &phent)) die("%s: program headers not mapped", g_exe_real.c_str());

  // Initial stack: strings, then (from sp up) argc, argv[], 0, envp[], 0, auxv[]
  uint64_t sbase; size_t ssize;
  armtr_cpu_stack(g_main_cpu, &sbase, &ssize);
  uint64_t top = sbase + ssize;
  auto push_bytes = [&](const void* p, size_t n) { top -= n; memcpy((void*)top, p, n); return top; };
  auto push_str = [&](const std::string& s) { return push_bytes(s.c_str(), s.size() + 1); };
  uint64_t execfn = push_str(g_exe_real);
  std::vector<uint64_t> envp, argp;
  for (size_t k = env.size(); k-- > 0;) envp.insert(envp.begin(), push_str(env[k]));
  for (size_t k = gargs.size(); k-- > 0;) argp.insert(argp.begin(), push_str(gargs[k]));
  uint64_t platform = push_str("aarch64");
  uint8_t rnd[16];
  if (getrandom(rnd, sizeof rnd, 0) != 16) memset(rnd, 0x5a, sizeof rnd);
  uint64_t random = push_bytes(rnd, 16);
  Auxv av;
  av.add(AT_PHDR, phdr); av.add(AT_PHENT, phent); av.add(AT_PHNUM, phnum); av.add(AT_PAGESZ, 4096);
  av.add(AT_BASE, lin ? armtr_image_base(lin) : 0); av.add(AT_FLAGS, 0); av.add(AT_ENTRY, armtr_image_entry(exe));
  av.add(AT_UID, getuid()); av.add(AT_EUID, geteuid()); av.add(AT_GID, getgid()); av.add(AT_EGID, getegid());
  av.add(AT_PLATFORM, platform); av.add(AT_HWCAP, hw1); av.add(AT_CLKTCK, 100); av.add(AT_SECURE, 0);
  av.add(AT_RANDOM, random); av.add(AT_HWCAP2, hw2); av.add(AT_EXECFN, execfn);
  av.add(51 /*AT_MINSIGSTKSZ*/, 5120);
  av.add(AT_NULL, 0);
  size_t words = 1 + argp.size() + 1 + envp.size() + 1 + av.v.size();
  top &= ~15ull;
  uint64_t sp = (top - words * 8) & ~15ull;
  uint64_t* w = (uint64_t*)sp;
  *w++ = argp.size();
  for (auto p : argp) *w++ = p;
  *w++ = 0;
  for (auto p : envp) *w++ = p;
  *w++ = 0;
  for (auto x : av.v) *w++ = x;

  armtr_regs r;
  memset(&r, 0, sizeof r);
  r.sp = sp;
  r.pc = armtr_image_entry(lin ? lin : exe);
  armtr_regs_set(g_main_cpu, &r);
  int rc = armtr_run(g_main_cpu, kNeverAddr);
  if (rc < 0) report_fault(g_main_cpu);
  // main thread called plain exit(): wait for the other threads like the kernel would
  while (g_threads.load() > 0) usleep(1000);
  fflush(nullptr);
  return g_main_exit & 0xff;
}
