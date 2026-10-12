// SPDX-License-Identifier: GPL-2.0-or-later
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include "t.h"
static std::atomic<int> g_futex{0};
static void* waiter(void*) {
  while (g_futex.load() == 0) {
    struct timespec ts = {5, 0};
    int64_t r = sc(A64_futex, P(&g_futex), 0 /*WAIT*/ | 128 /*PRIVATE*/, 0, P(&ts));
    if (r != 0 && r != -EAGAIN && r != -EINTR) { g_fail++; break; }
  }
  return nullptr;
}
static int hook_calls; static uint64_t hook_flags, hook_sp, hook_tls, hook_ctid, hook_ptid;
static int hook(const armtr_regs* r, uint64_t flags, uint64_t ctid, uint64_t ptid, void*) {
  hook_calls++; hook_flags = flags; hook_sp = r->sp; hook_tls = r->tpidr_el0; hook_ctid = ctid; hook_ptid = ptid; return 4242;
}
static int64_t icept(armtr_cpu*, uint64_t nr, const uint64_t a[6], void*) { return nr == A64_exit_group ? (int64_t)a[0] + 1000 : ARMTR_SYSCALL_PASS; }
int main() {
  // futex between two host threads
  pthread_t th; pthread_create(&th, nullptr, waiter, nullptr);
  usleep(100000);
  g_futex = 1;
  int64_t woken = 0;
  for (int i = 0; i < 50 && !woken; i++) { woken = sc(A64_futex, P(&g_futex), 1 /*WAKE*/ | 128, 1); if (!woken) usleep(10000); }
  CHECK_EQ(woken, 1);
  pthread_join(th, nullptr);
  CHECK_EQ(sc(A64_futex, P(&g_futex), 0 | 128, 0 /*mismatch*/), -EAGAIN);
  // time
  struct timespec ts, hts;
  CHECK_EQ(sc(A64_clock_gettime, CLOCK_MONOTONIC, P(&ts)), 0);
  clock_gettime(CLOCK_MONOTONIC, &hts); CHECK(hts.tv_sec - ts.tv_sec <= 1);
  CHECK_EQ(sc(A64_clock_getres, CLOCK_REALTIME, P(&ts)), 0);
  CHECK_EQ(sc(A64_clock_gettime, 9999, P(&ts)), -EINVAL);
  struct timeval tv; CHECK_EQ(sc(A64_gettimeofday, P(&tv), 0), 0); CHECK(tv.tv_sec > 1600000000);
  struct timespec sl = {0, 20000000}, t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  CHECK_EQ(sc(A64_nanosleep, P(&sl), 0), 0);
  CHECK_EQ(sc(A64_clock_nanosleep, CLOCK_MONOTONIC, 0, P(&sl), 0), 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) >= 39000000L);
  // ids
  CHECK_EQ(sc(A64_getpid), getpid()); CHECK_EQ(sc(A64_getppid), getppid());
  CHECK_EQ(sc(A64_gettid), syscall(SYS_gettid)); CHECK_EQ(sc(A64_getuid), getuid()); CHECK_EQ(sc(A64_getegid), getegid());
  CHECK_EQ(sc(A64_tgkill, getpid(), syscall(SYS_gettid), 0), 0);
  CHECK_EQ(sc(A64_kill, getpid(), 0), 0);
  CHECK_EQ(sc(A64_tkill, 0x7fffff00, 0), -ESRCH);
  CHECK_EQ(sc(A64_sched_yield), 0);
  // signals: guest handlers are only recorded
  uint64_t act[4] = {0x1234000, 4 | 0x04000000, 0x5678000, 0x2}, old[4] = {};
  CHECK_EQ(sc(A64_rt_sigaction, 10, P(act), 0, 8), 0);
  CHECK_EQ(sc(A64_rt_sigaction, 10, 0, P(old), 8), 0);
  CHECK(old[0] == 0x1234000 && old[1] == (4 | 0x04000000) && old[2] == 0x5678000 && old[3] == 2);
  armtr_guest_sigaction g; CHECK_EQ(armtr_syscalls_get_sigaction(10, &g), 0); CHECK_EQ(g.handler, 0x1234000);
  struct sigaction hsa; sigaction(SIGUSR1, nullptr, &hsa); CHECK(hsa.sa_handler == SIG_DFL);   // host untouched
  CHECK_EQ(sc(A64_rt_sigaction, 9, P(act), 0, 8), -EINVAL);
  CHECK_EQ(sc(A64_rt_sigaction, 10, P(act), 0, 4), -EINVAL);
  uint64_t ign[4] = {1, 0, 0, 0};
  CHECK_EQ(sc(A64_rt_sigaction, SIGUSR2, P(ign), 0, 8), 0);
  sigaction(SIGUSR2, nullptr, &hsa); CHECK(hsa.sa_handler == SIG_IGN);   // SIG_IGN is mirrored
  CHECK_EQ(sc(A64_kill, getpid(), SIGUSR2), 0);
  uint64_t set = 1ull << (SIGUSR1 - 1), oset = 0;
  CHECK_EQ(sc(A64_rt_sigprocmask, 0 /*BLOCK*/, P(&set), P(&oset), 8), 0);
  sigset_t cur; pthread_sigmask(SIG_BLOCK, nullptr, &cur); CHECK(sigismember(&cur, SIGUSR1));
  CHECK_EQ(sc(A64_rt_sigprocmask, 1 /*UNBLOCK*/, P(&set), 0, 8), 0);
  struct { void* sp; int fl; size_t sz; } ss = {(void*)0x1000, 0, 16384}, os;
  CHECK_EQ(sc(A64_sigaltstack, P(&ss), P(&os)), 0); CHECK_EQ(os.fl, 2);
  CHECK_EQ(sc(A64_sigaltstack, 0, P(&os)), 0); CHECK(os.sp == (void*)0x1000);
  ss.sz = 100; CHECK_EQ(sc(A64_sigaltstack, P(&ss), 0), -ENOMEM);
  // misc
  struct utsname u; CHECK_EQ(sc(A64_uname, P(&u)), 0); CHECK(!strcmp(u.machine, "aarch64")); CHECK(!strcmp(u.sysname, "Linux"));
  unsigned char rnd[16] = {}, z[16] = {};
  CHECK_EQ(sc(A64_getrandom, P(rnd), 16, 0), 16); CHECK(memcmp(rnd, z, 16));
  uint64_t rl[2]; CHECK_EQ(sc(A64_prlimit64, 0, RLIMIT_NOFILE, 0, P(rl)), 0); CHECK(rl[0] > 0);
  CHECK_EQ(sc(A64_getrlimit, RLIMIT_NOFILE, P(rl)), 0);
  uint64_t cpus[16] = {}; int64_t n = sc(A64_sched_getaffinity, 0, sizeof cpus, P(cpus)); CHECK(n > 0 && (cpus[0] & 1 || cpus[0]));
  CHECK_EQ(sc(A64_set_tid_address, P(&g_futex)), syscall(SYS_gettid));
  CHECK_EQ(sc(A64_prctl, 15 /*PR_SET_NAME*/, P("armtr")), 0);
  char nm[16] = {}; CHECK_EQ(sc(A64_prctl, 16, P(nm)), 0); CHECK(!strcmp(nm, "armtr"));
  CHECK_EQ(sc(A64_prctl, 0x7777, 0), -EINVAL);
  struct sysinfo_ { int64_t up; } si; uint8_t sib[128]; (void)si; CHECK_EQ(sc(A64_sysinfo, P(sib)), 0);
  uint64_t tms[4]; CHECK(sc(A64_times, P(tms)) != 0);
  CHECK(sc(A64_membarrier, 0, 0, 0) >= 0);   // CMD_QUERY
  CHECK_EQ(sc(A64_rseq, 0, 0, 0, 0), -ENOSYS);
  CHECK_EQ(sc(A64_seccomp, 0, 0, 0), -ENOSYS);
  CHECK_EQ(sc(A64_personality, 0x0008), -EINVAL);
  // clone: thread -> hook (no cpu: zero regs, sp/tls from args)
  armtr_syscalls_set_thread_hook(hook, nullptr);
  CHECK_EQ(sc(A64_clone, 0x3d0f00 /*VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|SETTLS|PARENT_SETTID|CHILD_CLEARTID*/, 0xAAA000, 0x111, 0xBBB000, 0x222), 4242);
  CHECK_EQ(hook_calls, 1); CHECK_EQ(hook_sp, 0xAAA000); CHECK_EQ(hook_tls, 0xBBB000); CHECK_EQ(hook_ctid, 0x222); CHECK_EQ(hook_ptid, 0x111);
  struct { uint64_t f, pidfd, ct, pt, es, st, ss, tls, a, b, c; } ca = {0x10100 | 0x80000, 0, 0x30, 0x40, 0, 0x100000, 0x1000, 0x77, 0, 0, 0};
  CHECK_EQ(sc(A64_clone3, P(&ca), sizeof ca), 4242);
  CHECK_EQ(hook_sp, 0x101000); CHECK_EQ(hook_tls, 0x77); CHECK_EQ(hook_ctid, 0x30);
  CHECK_EQ(sc(A64_clone3, P(&ca), 8), -EINVAL);
  armtr_syscalls_set_thread_hook(nullptr, nullptr);
  CHECK_EQ(sc(A64_clone, 0x10100 | 0x80000, 0x1000, 0, 0, 0), -ENOSYS);
  // clone as fork (SIGCHLD): child runs sc(exit_group) via raw path
  fflush(stdout);
  int64_t pid = sc(A64_clone, SIGCHLD, 0, 0, 0, 0);
  if (pid == 0) _exit(sc(A64_getpid) > 0 ? 7 : 8);
  CHECK(pid > 0);
  int st = 0; CHECK_EQ(sc(A64_wait4, pid, P(&st), 0, 0), pid); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
  // exit_group intercepted (embedder stops the guest instead of the host process)
  armtr_syscalls_set_intercept(icept, nullptr);
  CHECK_EQ(sc(A64_exit_group, 5), 1005);
  CHECK(sc(A64_getpid) == getpid());
  armtr_syscalls_set_intercept(nullptr, nullptr);
  DONE();
}
