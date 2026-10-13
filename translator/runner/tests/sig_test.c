// SPDX-License-Identifier: GPL-2.0-or-later
// Guest signal delivery under armtr-run (arm64 bionic, dynamic). Prints "<name> ok" or "<name> FAIL ..." per case.
// Modes: no arg = all cases; "dfl" = SIGUSR1 with SIG_DFL (dies by the signal); "segv" = unhandled null load.
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define ESR_MAGIC_ 0x45535201
#define FPSIMD_MAGIC_ 0x46508001

static void report(const char* name, int ok, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#include <stdarg.h>
static void report(const char* name, int ok, const char* fmt, ...) {
  if (ok) { printf("%s ok\n", name); return; }
  printf("%s FAIL ", name);
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
  printf("\n");
}

static void set(int sig, void* fn, int flags, int masksig) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))fn;
  sa.sa_flags = flags | SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  if (masksig) sigaddset(&sa.sa_mask, masksig);
  sigaction(sig, &sa, NULL);
}
static int blocked(int sig) { sigset_t m; sigprocmask(SIG_BLOCK, NULL, &m); return sigismember(&m, sig); }

// ---- async: raise() + siginfo + sa_mask ----
static volatile int g_sig, g_b1, g_b2;
static siginfo_t g_si;
static void h_info(int s, siginfo_t* si, void* uc) { g_sig = s; g_si = *si; g_b1 = blocked(SIGUSR1); g_b2 = blocked(SIGUSR2); (void)uc; }

static void t_info(void) {
  set(SIGUSR1, h_info, 0, SIGUSR2);
  raise(SIGUSR1);
  report("raise_siginfo_mask", g_sig == SIGUSR1 && g_si.si_code == SI_TKILL && g_si.si_pid == getpid() && g_b1 && g_b2 &&
         !blocked(SIGUSR1) && !blocked(SIGUSR2), "sig=%d code=%d pid=%d b=%d%d after=%d%d", g_sig, g_si.si_code,
         g_si.si_pid, g_b1, g_b2, blocked(SIGUSR1), blocked(SIGUSR2));
  g_sig = 0;
  set(SIGUSR1, h_info, SA_NODEFER | SA_RESETHAND, 0);
  kill(getpid(), SIGUSR1);
  struct sigaction cur;
  sigaction(SIGUSR1, NULL, &cur);
  report("nodefer_resethand", g_sig == SIGUSR1 && !g_b1 && g_si.si_code == SI_USER && cur.sa_handler == SIG_DFL,
         "sig=%d b1=%d code=%d dfl=%d", g_sig, g_b1, g_si.si_code, cur.sa_handler == SIG_DFL);
}

// ---- all NEON/FP registers, temporaries and NZCV survive a handler that clobbers them (rt_sigreturn) ----
static void h_clobber(int s, siginfo_t* si, void* uc) {
  (void)s; (void)si; (void)uc;
  __asm__ volatile(
#define M(n) "movi v" #n ".16b, #0xee\n"
      M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)
      M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23) M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)
#undef M
      "mov x9, #0\nmov x10, #0\nmov x11, #0\nmov x12, #0\nmov x13, #0\nmov x14, #0\nmov x15, #0\nmov x16, #1\ncmp x16, #2\n"
      ::: "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16",
      "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "x9",
      "x10", "x11", "x12", "x13", "x14", "x15", "x16", "cc");
}

static void t_regs(void) {
  static uint8_t v[32][16];
  static uint64_t x[8];
  set(SIGUSR2, h_clobber, 0, 0);
  register long x0 __asm__("x0") = getpid();
  register long x1 __asm__("x1") = gettid();
  register long x2 __asm__("x2") = SIGUSR2;
  register long x8 __asm__("x8") = __NR_tgkill;
  __asm__ volatile(
#define M(n, i) "movi v" #n ".16b, #" #i "\n"
      M(0, 1) M(1, 2) M(2, 3) M(3, 4) M(4, 5) M(5, 6) M(6, 7) M(7, 8) M(8, 9) M(9, 10) M(10, 11) M(11, 12)
      M(12, 13) M(13, 14) M(14, 15) M(15, 16) M(16, 17) M(17, 18) M(18, 19) M(19, 20) M(20, 21) M(21, 22)
      M(22, 23) M(23, 24) M(24, 25) M(25, 26) M(26, 27) M(27, 28) M(28, 29) M(29, 30) M(30, 31) M(31, 32)
#undef M
      "mov x9, #9\nmov x10, #10\nmov x11, #11\nmov x12, #12\nmov x13, #13\nmov x14, #14\nmov x15, #15\n"
      "cmp x9, x9\n"
      "svc #0\n"
      "cset x16, eq\n"
      "mov x17, %[v]\n"
      "stp q0, q1, [x17], #32\nstp q2, q3, [x17], #32\nstp q4, q5, [x17], #32\nstp q6, q7, [x17], #32\n"
      "stp q8, q9, [x17], #32\nstp q10, q11, [x17], #32\nstp q12, q13, [x17], #32\nstp q14, q15, [x17], #32\n"
      "stp q16, q17, [x17], #32\nstp q18, q19, [x17], #32\nstp q20, q21, [x17], #32\nstp q22, q23, [x17], #32\n"
      "stp q24, q25, [x17], #32\nstp q26, q27, [x17], #32\nstp q28, q29, [x17], #32\nstp q30, q31, [x17], #32\n"
      "stp x9, x10, [%[x]]\nstp x11, x12, [%[x], #16]\nstp x13, x14, [%[x], #32]\nstp x15, x16, [%[x], #48]\n"
      : "+r"(x0)
      : "r"(x1), "r"(x2), "r"(x8), [v] "r"(v), [x] "r"(x)
      : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16",
        "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31",
        "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
  int bad = -1;
  for (int i = 0; i < 32 && bad < 0; i++)
    for (int k = 0; k < 16; k++) if (v[i][k] != i + 1) { bad = i; break; }
  int xok = x[7] == 1;
  for (int i = 0; i < 7; i++) xok &= x[i] == (uint64_t)(9 + i);
  report("regs_preserved", bad < 0 && xok && x0 == 0, "bad v%d x9=%llu flags=%llu ret=%ld", bad,
         (unsigned long long)x[0], (unsigned long long)x[7], x0);
}

// ---- SIGALRM while the guest spins without syscalls (kick inside translated code) ----
static volatile int g_alarm;
static void h_alarm(int s, siginfo_t* si, void* uc) { (void)s; (void)si; (void)uc; g_alarm++; }
static void t_alarm(void) {
  set(SIGALRM, h_alarm, 0, 0);
  struct itimerval it = {{0, 0}, {0, 20000}};
  setitimer(ITIMER_REAL, &it, NULL);
  volatile unsigned long spin = 0;
  while (!g_alarm) spin++;
  report("alarm_busy_loop", g_alarm == 1 && spin > 0, "alarm=%d", g_alarm);
}

// ---- timer storm: handlers clobbering all FP/NEON regs land at arbitrary points of an FP/integer loop ----
static volatile int g_ticks;
static void h_tick(int s, siginfo_t* si, void* uc) { g_ticks++; h_clobber(s, si, uc); }
static double work(unsigned n) {
  double d = 0; unsigned long x = 1;
  for (unsigned i = 1; i < n; i++) { x = x * 6364136223846793005ul + i; d += (double)(x >> 40) / i; if (!(i & 4095)) getppid(); }
  return d + (double)(x & 0xffff);
}
static void t_storm(void) {
  double ref = work(3000000);
  set(SIGALRM, h_tick, SA_RESTART, 0);
  struct itimerval it = {{0, 500}, {0, 500}}, off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_REAL, &it, NULL);
  double r1 = work(3000000), r2 = work(3000000);
  setitimer(ITIMER_REAL, &off, NULL);
  report("timer_storm", r1 == ref && r2 == ref && g_ticks > 10, "ticks=%d ref=%.17g r=%.17g %.17g", g_ticks, ref, r1, r2);
}

// ---- SA_RESTART vs EINTR for a blocked read() in another thread (pthread_kill) ----
static int g_pipe[2];
static volatile int g_hits;
static long g_rd, g_err;
static void h_count(int s, siginfo_t* si, void* uc) { (void)s; (void)si; (void)uc; g_hits++; }
static void* reader(void* p) {
  char c;
  (void)p;
  g_rd = read(g_pipe[0], &c, 1);
  g_err = g_rd < 0 ? errno : 0;
  return NULL;
}
static void t_restart(int flags, const char* name, long want_rd, long want_err) {
  g_hits = 0;
  set(SIGUSR2, h_count, flags, 0);
  pipe(g_pipe);
  pthread_t th;
  pthread_create(&th, NULL, reader, NULL);
  usleep(100000);
  pthread_kill(th, SIGUSR2);
  usleep(100000);
  write(g_pipe[1], "x", 1);
  pthread_join(th, NULL);
  close(g_pipe[0]); close(g_pipe[1]);
  report(name, g_hits == 1 && g_rd == want_rd && g_err == want_err, "hits=%d rd=%ld errno=%ld", g_hits, g_rd, g_err);
}

// ---- synchronous faults: exact pc / si_addr / si_code / esr, handler skips the insn ----
static volatile uint64_t g_pc, g_addr, g_fa, g_esr;
static volatile int g_code, g_fpsimd, g_signo;
static void h_fault(int s, siginfo_t* si, void* ucv) {
  ucontext_t* uc = (ucontext_t*)ucv;
  g_signo = s; g_code = si->si_code; g_addr = (uint64_t)si->si_addr;
  g_pc = uc->uc_mcontext.pc; g_fa = uc->uc_mcontext.fault_address;
  g_esr = 0; g_fpsimd = 0;
  for (uint32_t* h = (uint32_t*)uc->uc_mcontext.__reserved; h[0]; h = (uint32_t*)((char*)h + h[1])) {
    if (h[0] == ESR_MAGIC_) g_esr = *(uint64_t*)(h + 2);
    if (h[0] == FPSIMD_MAGIC_ && h[1] == 528) g_fpsimd = 1;
  }
  uc->uc_mcontext.pc += 4;
}

static uint64_t g_cnt;
// One faulting load/store at label 1; the following insns must run exactly once (after the handler).
static uint64_t access_at(uint64_t addr, int write, uint64_t* val) {
  uint64_t pc, v = 0x5a, t;
  if (write)
    __asm__ volatile("adr %[pc], 1f\n1: str %[v], [%[a]]\nldr %[t], [%[c]]\nadd %[t], %[t], #1\nstr %[t], [%[c]]\n"
                     : [pc] "=&r"(pc), [t] "=&r"(t) : [a] "r"(addr), [v] "r"(v), [c] "r"(&g_cnt) : "memory");
  else
    __asm__ volatile("adr %[pc], 1f\n1: ldr %[v], [%[a]]\nldr %[t], [%[c]]\nadd %[t], %[t], #1\nstr %[t], [%[c]]\n"
                     : [pc] "=&r"(pc), [t] "=&r"(t), [v] "+r"(v) : [a] "r"(addr), [c] "r"(&g_cnt) : "memory");
  *val = v;
  return pc;
}

static void fault_case(const char* name, uint64_t addr, int write, int code) {
  g_cnt = 0; g_signo = 0;
  uint64_t v, pc = access_at(addr, write, &v);
  int ec = (int)(g_esr >> 26), wnr = (int)(g_esr >> 6 & 1);
  report(name, g_signo == SIGSEGV && g_code == code && g_addr == addr && g_pc == pc && g_fa == addr && ec == 0x24 &&
         wnr == write && g_fpsimd && g_cnt == 1 && v == 0x5a,
         "sig=%d code=%d addr=%llx/%llx pc=%llx/%llx fa=%llx esr=%llx fp=%d cnt=%llu v=%llx", g_signo, g_code,
         (unsigned long long)g_addr, (unsigned long long)addr, (unsigned long long)g_pc, (unsigned long long)pc,
         (unsigned long long)g_fa, (unsigned long long)g_esr, g_fpsimd, (unsigned long long)g_cnt,
         (unsigned long long)v);
}

static void t_faults(void) {
  set(SIGSEGV, h_fault, 0, 0);
  set(SIGILL, h_fault, 0, 0);
  set(SIGTRAP, h_fault, 0, 0);
  fault_case("segv_null_read", 8, 0, SEGV_MAPERR);
  uint8_t* p = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  munmap(p + 4096, 4096);
  fault_case("segv_unmapped_read", (uint64_t)p + 4096 + 16, 0, SEGV_MAPERR);
  mprotect(p, 4096, PROT_NONE);
  fault_case("segv_prot_write", (uint64_t)p + 24, 1, SEGV_ACCERR);
  p[2 * 4096] = 1;   // TLB entry now hot: the next fault is in translated code itself
  mprotect(p + 2 * 4096, 4096, PROT_READ);
  fault_case("segv_hot_write", (uint64_t)p + 2 * 4096 + 32, 1, SEGV_ACCERR);
  munmap(p, 3 * 4096);

  uint64_t pc;
  g_signo = 0;
  __asm__ volatile("adr %0, 1f\n1: .inst 0x00000000\n" : "=r"(pc));
  report("sigill_udf", g_signo == SIGILL && g_code == ILL_ILLOPC && g_addr == pc && g_pc == pc && !g_esr,
         "sig=%d code=%d addr=%llx pc=%llx/%llx", g_signo, g_code, (unsigned long long)g_addr,
         (unsigned long long)g_pc, (unsigned long long)pc);
  g_signo = 0;
  __asm__ volatile("adr %0, 1f\n1: brk #0x3e8\n" : "=r"(pc));
  report("sigtrap_brk", g_signo == SIGTRAP && g_code == TRAP_BRKPT && g_addr == pc && g_pc == pc,
         "sig=%d code=%d addr=%llx pc=%llx/%llx", g_signo, g_code, (unsigned long long)g_addr,
         (unsigned long long)g_pc, (unsigned long long)pc);
}

// ---- stack overflow in a thread -> SIGSEGV on the sigaltstack (SA_ONSTACK) -> siglongjmp out ----
static sigjmp_buf g_jb;
static char* g_alt;
static volatile int g_on_alt;
static void h_ovf(int s, siginfo_t* si, void* uc) {
  char here;
  (void)s; (void)si; (void)uc;
  g_on_alt = &here >= g_alt && &here < g_alt + 65536;
  siglongjmp(g_jb, 1);
}
static int recurse(volatile char* p) { volatile char buf[1024]; buf[0] = p ? p[0] : 0; return recurse(buf) + buf[1]; }
static void* ovf_thread(void* arg) {
  (void)arg;
  g_alt = malloc(65536);
  stack_t ss = {.ss_sp = g_alt, .ss_size = 65536, .ss_flags = 0};
  sigaltstack(&ss, NULL);
  set(SIGSEGV, h_ovf, SA_ONSTACK, 0);
  if (!sigsetjmp(g_jb, 1)) recurse(NULL);
  return (void*)(long)(g_on_alt && !blocked(SIGSEGV));
}
static void t_overflow(void) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 128 * 1024);
  pthread_t th;
  void* r = 0;
  pthread_create(&th, &a, ovf_thread, NULL);
  pthread_join(th, &r);
  report("stack_overflow_altstack", r == (void*)1, "on_alt=%d", g_on_alt);
}

// ---- SIGPIPE (handler, then SIG_IGN) and SIGCHLD ----
static void t_pipe_chld(void) {
  g_sig = 0;
  set(SIGPIPE, h_info, 0, 0);
  int p[2];
  pipe(p); close(p[0]);
  long r = write(p[1], "x", 1);
  int e = errno;
  report("sigpipe_handler", g_sig == SIGPIPE && r == -1 && e == EPIPE, "sig=%d r=%ld errno=%d", g_sig, r, e);
  signal(SIGPIPE, SIG_IGN);
  r = write(p[1], "x", 1);
  e = errno;
  close(p[1]);
  report("sigpipe_ignored", r == -1 && e == EPIPE, "r=%ld errno=%d", r, e);

  g_sig = 0;
  set(SIGCHLD, h_info, 0, 0);
  pid_t pid = fork();
  if (pid == 0) _exit(3);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  report("sigchld", g_sig == SIGCHLD && g_si.si_pid == pid && g_si.si_code == CLD_EXITED && WEXITSTATUS(st) == 3,
         "sig=%d pid=%d/%d code=%d st=%x", g_sig, g_si.si_pid, pid, g_si.si_code, st);
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 1 && !strcmp(argv[1], "dfl")) { raise(SIGUSR1); return 0; }
  if (argc > 1 && !strcmp(argv[1], "segv")) return *(volatile int*)8;
  t_info();
  t_regs();
  t_alarm();
  t_storm();
  t_restart(SA_RESTART, "read_sa_restart", 1, 0);
  t_restart(0, "read_eintr", -1, EINTR);
  t_faults();
  t_overflow();
  t_pipe_chld();
  printf("done\n");
  return 0;
}
