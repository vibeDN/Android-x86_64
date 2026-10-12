// SPDX-License-Identifier: GPL-2.0-or-later
// armtr syscalls: aarch64 Linux syscall -> x86_64 host Linux syscall, in-process, identity memory.
//
// Design: the table of aarch64 numbers is generated from the kernel (a64_table.inc). Every entry with an
// x86_64 syscall of the same name whose argument layout is identical on both ABIs is passed through
// unchanged (the default). Syscalls whose ABI differs (open flags, struct stat, epoll_event, clone,
// signals, mmap bookkeeping, ...) have a custom handler in custom() below. Results are raw kernel
// results (negative errno), no libc errno involved.
#include <asm/unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "armtr_syscalls_ext.h"
#include "a64_nr.h"
#include "host.h"

// Optional engine hooks (engine may not be linked, e.g. in unit tests): weak.
extern "C" {
int armtr_mem_map(uint64_t addr, size_t len, int prot) __attribute__((weak));
int armtr_mem_unmap(uint64_t addr, size_t len) __attribute__((weak));
int armtr_mem_protect(uint64_t addr, size_t len, int prot) __attribute__((weak));
void armtr_regs_get(armtr_cpu*, armtr_regs*) __attribute__((weak));
}

namespace {

constexpr int64_t kENOSYS = -38, kEINVAL = -22, kEPERM = -1, kE2BIG = -7, kEFAULT = -14, kENOMEM = -12;

// ---- state ----
struct Entry { int nr; const char* name; int host; };
const Entry kTable[] = {
#include "a64_table.inc"
};
constexpr size_t kMaxNr = 512;
const Entry* g_entry[kMaxNr];
std::once_flag g_once;
std::atomic<uint8_t> g_warned[kMaxNr];

armtr_thread_start_fn g_thread_fn;
void* g_thread_user;
armtr_syscall_intercept_fn g_icept;
void* g_icept_user;
thread_local bool t_in_icept;

std::mutex g_sig_mu;
armtr_guest_sigaction g_sigs[65];
struct GStack { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; };
thread_local GStack t_altstack = {0, 2 /*SS_DISABLE*/, 0, 0};

void init() {
  for (const Entry& e : kTable) if ((size_t)e.nr < kMaxNr) g_entry[e.nr] = &e;
}

// ---- guest memory access (guest pointers are host pointers, but report -EFAULT instead of crashing) ----
bool copy_raw(bool out, void* guest, void* local, size_t n) {
  if (!n) return true;
  struct iov { void* b; size_t l; } l{local, n}, r{guest, n};
  long pid = rawsc(__NR_getpid);
  long rc = rawsc(out ? __NR_process_vm_writev : __NR_process_vm_readv, pid, (long)&l, 1, (long)&r, 1, 0);
  if (rc == (long)n) return true;
  if (rc == -38 || rc == -1 || rc == -13) {   // ENOSYS/EPERM/EACCES: sandboxed, trust the pointer
    if (!guest) return false;
    if (out) memcpy(guest, local, n); else memcpy(local, guest, n);
    return true;
  }
  return false;
}
bool get_user(void* dst, uint64_t g, size_t n) { return copy_raw(false, (void*)g, dst, n); }
bool put_user(uint64_t g, const void* src, size_t n) { return copy_raw(true, (void*)g, (void*)src, n); }

// ---- flag conversion ----
// aarch64 (asm-generic + arch/arm64 fcntl.h) vs x86_64 (asm-generic only) differ in four O_* bits.
constexpr uint32_t A_DIRECTORY = 040000, A_NOFOLLOW = 0100000, A_DIRECT = 0200000, A_LARGEFILE = 0400000;
constexpr uint32_t X_DIRECT = 040000, X_LARGEFILE = 0100000, X_DIRECTORY = 0200000, X_NOFOLLOW = 0400000;
constexpr uint32_t kAMask = A_DIRECTORY | A_NOFOLLOW | A_DIRECT | A_LARGEFILE;
constexpr uint32_t kXMask = X_DIRECT | X_LARGEFILE | X_DIRECTORY | X_NOFOLLOW;
uint64_t o_a2x(uint64_t f) {
  uint64_t r = f & ~(uint64_t)kAMask;
  if (f & A_DIRECTORY) r |= X_DIRECTORY;
  if (f & A_NOFOLLOW) r |= X_NOFOLLOW;
  if (f & A_DIRECT) r |= X_DIRECT;
  if (f & A_LARGEFILE) r |= X_LARGEFILE;
  return r;
}
uint64_t o_x2a(uint64_t f) {
  uint64_t r = f & ~(uint64_t)kXMask;
  if (f & X_DIRECTORY) r |= A_DIRECTORY;
  if (f & X_NOFOLLOW) r |= A_NOFOLLOW;
  if (f & X_DIRECT) r |= A_DIRECT;
  if (f & X_LARGEFILE) r |= A_LARGEFILE;
  return r;
}

// ---- struct stat: x86_64 (arch/x86/include/uapi/asm/stat.h) -> aarch64 (asm-generic/stat.h) ----
struct X86Stat {
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, pad0;
  uint64_t st_rdev;
  int64_t st_size, st_blksize, st_blocks;
  uint64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
  int64_t unused[3];
};
struct A64Stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, pad1;
  int64_t st_size;
  int32_t st_blksize, pad2;
  int64_t st_blocks;
  int64_t atime; uint64_t atime_nsec;
  int64_t mtime; uint64_t mtime_nsec;
  int64_t ctime; uint64_t ctime_nsec;
  uint32_t unused4, unused5;
};
static_assert(sizeof(X86Stat) == 144 && sizeof(A64Stat) == 128, "stat layouts");
int64_t put_stat(uint64_t dst, const X86Stat& x) {
  A64Stat a;
  memset(&a, 0, sizeof a);
  a.st_dev = x.st_dev; a.st_ino = x.st_ino; a.st_mode = x.st_mode; a.st_nlink = (uint32_t)x.st_nlink;
  a.st_uid = x.st_uid; a.st_gid = x.st_gid; a.st_rdev = x.st_rdev; a.st_size = x.st_size;
  a.st_blksize = (int32_t)x.st_blksize; a.st_blocks = x.st_blocks;
  a.atime = x.atime; a.atime_nsec = x.atime_nsec; a.mtime = x.mtime; a.mtime_nsec = x.mtime_nsec;
  a.ctime = x.ctime; a.ctime_nsec = x.ctime_nsec;
  return put_user(dst, &a, sizeof a) ? 0 : kEFAULT;
}

// ---- epoll_event: x86_64 is packed (12 bytes), aarch64 is {u32, pad, u64} (16 bytes) ----
struct __attribute__((packed)) XEv { uint32_t events; uint64_t data; };
struct AEv { uint32_t events, pad; uint64_t data; };

int64_t epoll_wait_common(const uint64_t* a, bool pwait2) {
  int maxev = (int)a[2];
  if (maxev <= 0 || maxev > (1 << 20)) return kEINVAL;
  std::vector<XEv> buf((size_t)maxev);
  long rc = rawsc(pwait2 ? __NR_epoll_pwait2 : __NR_epoll_pwait, (long)a[0], (long)buf.data(), maxev, (long)a[3], (long)a[4], (long)a[5]);
  if (rc > 0) {
    std::vector<AEv> out((size_t)rc);
    for (long i = 0; i < rc; i++) out[i] = AEv{buf[i].events, 0, buf[i].data};
    if (!put_user(a[1], out.data(), out.size() * sizeof(AEv))) return kEFAULT;
  }
  return rc;
}

// ---- mmap family ----
constexpr uint64_t kMapX86Only = 0x40 | 0x80;     // MAP_32BIT | MAP_ABOVE4G are x86-only bit values
constexpr uint64_t kProtArm = 0x10 | 0x20;        // PROT_BTI | PROT_MTE
void note_map(uint64_t addr, uint64_t len, uint64_t prot) { if (armtr_mem_map) armtr_mem_map(addr, len, (int)(prot & 7)); }

// ---- clone ----
constexpr uint64_t CL_VM = 0x100, CL_VFORK = 0x4000, CL_THREAD = 0x10000, CL_SETTLS = 0x80000;

int64_t spawn_thread(armtr_cpu* cpu, uint64_t flags, uint64_t stack, uint64_t ptid, uint64_t tls, uint64_t ctid) {
  if (!g_thread_fn) return kENOSYS;
  armtr_regs r;
  memset(&r, 0, sizeof r);
  if (cpu && armtr_regs_get) armtr_regs_get(cpu, &r);   // pc is expected to be already past the svc
  r.x[0] = 0;
  if (stack) r.sp = stack;
  if (flags & CL_SETTLS) r.tpidr_el0 = tls;
  return g_thread_fn(&r, flags, ctid, ptid, g_thread_user);
}

int64_t do_clone(armtr_cpu* cpu, const uint64_t* a) {
  uint64_t flags = a[0];
  if (flags & CL_THREAD) return spawn_thread(cpu, flags, a[1], a[2], a[3], a[4]);
  // fork / vfork semantics: the child keeps running on the parent's (copied) stack inside the engine, so
  // the guest sp/tls are not host state. CL_VM|CL_VFORK degrades to a plain fork (exec follows).
  flags &= ~(CL_VM | CL_VFORK | CL_SETTLS);
  return rawsc(__NR_clone, (long)flags, 0, (long)a[2], (long)a[4], 0);   // x86 order: flags,stack,ptid,ctid,tls
}

struct CloneArgs { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls, set_tid, set_tid_size, cgroup; };
int64_t do_clone3(armtr_cpu* cpu, const uint64_t* a) {
  size_t sz = a[1];
  if (sz < 64) return kEINVAL;
  if (sz > sizeof(CloneArgs)) return kE2BIG;
  CloneArgs ca;
  memset(&ca, 0, sizeof ca);
  if (!get_user(&ca, a[0], sz)) return kEFAULT;
  if (ca.flags & CL_THREAD)
    return spawn_thread(cpu, ca.flags, ca.stack ? ca.stack + ca.stack_size : 0, ca.parent_tid, ca.tls, ca.child_tid);
  ca.flags &= ~(CL_VM | CL_VFORK | CL_SETTLS);
  ca.stack = ca.stack_size = ca.tls = 0;
  return rawsc(__NR_clone3, (long)&ca, (long)sz);
}

// ---- signals ----
int64_t do_sigaction(const uint64_t* a) {
  int sig = (int)a[0];
  if (a[3] != 8 || sig < 1 || sig > 64) return kEINVAL;
  if (a[1] && (sig == 9 || sig == 19)) return kEINVAL;
  armtr_guest_sigaction nw{};
  if (a[1]) {   // kernel struct: handler, flags, restorer, mask
    uint64_t k[4];
    if (!get_user(k, a[1], sizeof k)) return kEFAULT;
    nw = {k[0], k[1], k[2], k[3]};
  }
  std::lock_guard<std::mutex> g(g_sig_mu);
  armtr_guest_sigaction old = g_sigs[sig];
  if (a[2]) {
    uint64_t k[4] = {old.handler, old.flags, old.restorer, old.mask};
    if (!put_user(a[2], k, sizeof k)) return kEFAULT;
  }
  if (a[1]) {
    g_sigs[sig] = nw;
    // Only SIG_IGN is mirrored to the host (so ignored SIGPIPE etc. really are ignored). Real guest
    // handlers stay in the table for the embedder; the host disposition is untouched.
    if (nw.handler == 1 || (nw.handler == 0 && old.handler == 1)) {
      uint64_t hk[4] = {nw.handler, nw.flags & ~0x04000000ull, 0, nw.mask};
      rawsc(__NR_rt_sigaction, sig, (long)hk, 0, 8);
    }
  }
  return 0;
}

int64_t do_sigaltstack(const uint64_t* a) {
  if (a[1]) { if (!put_user(a[1], &t_altstack, sizeof(GStack))) return kEFAULT; }
  if (a[0]) {
    GStack n;
    if (!get_user(&n, a[0], sizeof n)) return kEFAULT;
    if (!(n.flags & 2) && n.size < 5120 /*arm64 MINSIGSTKSZ*/) return kENOMEM;
    t_altstack = n;
  }
  return 0;
}

// ---- custom handlers ----
bool custom(armtr_cpu* cpu, uint64_t nr, const uint64_t* a, int64_t* res) {
  auto R = [&](long v) { *res = v; return true; };
  switch (nr) {
    case A64_openat: return R(rawsc(__NR_openat, (long)a[0], (long)a[1], (long)o_a2x(a[2]), (long)a[3]));
    case A64_openat2: {
      uint64_t how[16];
      if (a[3] < 24) return R(kEINVAL);
      if (a[3] > sizeof how) return R(kE2BIG);
      memset(how, 0, sizeof how);
      if (!get_user(how, a[2], a[3])) return R(kEFAULT);
      how[0] = o_a2x(how[0]);
      return R(rawsc(__NR_openat2, (long)a[0], (long)a[1], (long)how, (long)a[3]));
    }
    case A64_mq_open: return R(rawsc(__NR_mq_open, (long)a[0], (long)o_a2x(a[1]), (long)a[2], (long)a[3]));
    case A64_open_by_handle_at: return R(rawsc(__NR_open_by_handle_at, (long)a[0], (long)a[1], (long)o_a2x(a[2])));
    case A64_fanotify_init: return R(rawsc(__NR_fanotify_init, (long)a[0], (long)o_a2x(a[1])));
    case A64_pipe2: return R(rawsc(__NR_pipe2, (long)a[0], (long)o_a2x(a[1])));
    case A64_fcntl: {
      constexpr uint64_t F_GETFL = 3, F_SETFL = 4;
      if (a[1] == F_SETFL) return R(rawsc(__NR_fcntl, (long)a[0], F_SETFL, (long)o_a2x(a[2])));
      long r = rawsc(__NR_fcntl, (long)a[0], (long)a[1], (long)a[2]);
      return R(a[1] == F_GETFL && r >= 0 ? (long)o_x2a((uint64_t)r) : r);
    }
    case A64_fstat: {
      X86Stat x;
      long r = rawsc(__NR_fstat, (long)a[0], (long)&x);
      return R(r < 0 ? r : put_stat(a[1], x));
    }
    case A64_newfstatat: {
      X86Stat x;
      long r = rawsc(__NR_newfstatat, (long)a[0], (long)a[1], (long)&x, (long)a[3]);
      return R(r < 0 ? r : put_stat(a[2], x));
    }
    case A64_epoll_ctl: {
      if (!a[3]) return R(rawsc(__NR_epoll_ctl, (long)a[0], (long)a[1], (long)a[2], 0));
      AEv ae;
      if (!get_user(&ae, a[3], sizeof ae)) return R(kEFAULT);
      XEv xe{ae.events, ae.data};
      return R(rawsc(__NR_epoll_ctl, (long)a[0], (long)a[1], (long)a[2], (long)&xe));
    }
    case A64_epoll_pwait: return R(epoll_wait_common(a, false));
    case A64_epoll_pwait2: return R(epoll_wait_common(a, true));
    case A64_mmap: {
      long r = rawsc(__NR_mmap, (long)a[0], (long)a[1], (long)(a[2] & ~kProtArm), (long)(a[3] & ~kMapX86Only), (long)a[4], (long)a[5]);
      if (r >= 0) note_map((uint64_t)r, a[1], a[2]);
      return R(r);
    }
    case A64_munmap: {
      long r = rawsc(__NR_munmap, (long)a[0], (long)a[1]);
      if (r == 0 && armtr_mem_unmap) armtr_mem_unmap(a[0], a[1]);
      return R(r);
    }
    case A64_mprotect: {
      long r = rawsc(__NR_mprotect, (long)a[0], (long)a[1], (long)(a[2] & ~kProtArm));
      if (r == 0 && armtr_mem_protect) armtr_mem_protect(a[0], a[1], (int)(a[2] & 7));
      return R(r);
    }
    case A64_pkey_mprotect: return R(rawsc(__NR_pkey_mprotect, (long)a[0], (long)a[1], (long)(a[2] & ~kProtArm), (long)a[3]));
    case A64_mremap: {
      long r = rawsc(__NR_mremap, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4]);
      if (r >= 0) {
        if ((uint64_t)r != a[0]) { if (armtr_mem_unmap) armtr_mem_unmap(a[0], a[1]); }
        else if (a[2] < a[1] && armtr_mem_unmap) armtr_mem_unmap(a[0] + a[2], a[1] - a[2]);
        note_map((uint64_t)r, a[2], 7);   // ponytail: prot not tracked, engine gets RWX; track per-region if it matters
      }
      return R(r);
    }
    case A64_brk: return R(0);   // bionic/glibc fall back to mmap when brk fails; never touch the host heap
    case A64_clone: return R(do_clone(cpu, a));
    case A64_clone3: return R(do_clone3(cpu, a));
    case A64_rt_sigaction: return R(do_sigaction(a));
    case A64_sigaltstack: return R(do_sigaltstack(a));
    case A64_rt_sigreturn: return R(kENOSYS);   // embedder intercept restores the guest signal frame
    case A64_uname: {
      long r = rawsc(__NR_uname, (long)a[0]);
      if (r == 0) { static const char m[] = "aarch64"; if (!put_user(a[0] + 4 * 65, m, sizeof m)) return R(kEFAULT); }
      return R(r);
    }
    case A64_personality: return R(a[0] == 0 || a[0] == 0xffffffffu ? rawsc(__NR_personality, (long)a[0]) : kEINVAL);
    case A64_rseq:         // host libc owns the thread's rseq registration
    case A64_seccomp:      // a filter would be checked against the host ABI/numbers
    case A64_restart_syscall:
    case A64_lookup_dcookie:
    case A64_nfsservctl: return R(kENOSYS);
    case A64_reboot: case A64_kexec_load: case A64_kexec_file_load: case A64_init_module: case A64_finit_module:
    case A64_delete_module: case A64_swapon: case A64_swapoff: return R(kEPERM);
    case A64_exit: return R(rawsc(__NR_exit, (long)a[0]));   // embedder should intercept; see header
    case A64_exit_group: return R(rawsc(__NR_exit_group, (long)a[0]));
    default: return false;
  }
}

}  // namespace

extern "C" {

void armtr_syscalls_set_thread_hook(armtr_thread_start_fn fn, void* user) { g_thread_fn = fn; g_thread_user = user; }
void armtr_syscalls_set_intercept(armtr_syscall_intercept_fn fn, void* user) { g_icept = fn; g_icept_user = user; }

int armtr_syscalls_get_sigaction(int sig, armtr_guest_sigaction* out) {
  if (sig < 1 || sig > 64) return -22;
  std::lock_guard<std::mutex> g(g_sig_mu);
  *out = g_sigs[sig];
  return 0;
}

const char* armtr_syscall_name(uint64_t nr) {
  std::call_once(g_once, init);
  if (nr >= kMaxNr || !g_entry[nr]) return nullptr;
  const Entry* e = g_entry[nr];
  if (e->host < 0) return nullptr;   // no x86_64 counterpart: not implemented
  return e->name;
}

int armtr_syscall_host_nr(uint64_t nr) {
  std::call_once(g_once, init);
  return nr < kMaxNr && g_entry[nr] ? g_entry[nr]->host : -1;
}

int64_t armtr_syscall(armtr_cpu* cpu, uint64_t nr, const uint64_t args[6], void* user) {
  (void)user;
  std::call_once(g_once, init);
  if (g_icept && !t_in_icept) {
    t_in_icept = true;
    int64_t r = g_icept(cpu, nr, args, g_icept_user);
    t_in_icept = false;
    if (r != ARMTR_SYSCALL_PASS) return r;
  }
  const Entry* e = nr < kMaxNr ? g_entry[nr] : nullptr;
  if (e) {
    int64_t res;
    if (custom(cpu, nr, args, &res)) return res;
    if (e->host >= 0)
      return rawsc(e->host, (long)args[0], (long)args[1], (long)args[2], (long)args[3], (long)args[4], (long)args[5]);
  }
  if (nr < kMaxNr ? !g_warned[nr].exchange(1) : true)
    fprintf(stderr, "armtr: unimplemented aarch64 syscall %llu (%s)\n", (unsigned long long)nr, e ? e->name : "?");
  return kENOSYS;
}

}  // extern "C"
