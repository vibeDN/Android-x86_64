// SPDX-License-Identifier: GPL-2.0-or-later
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <atomic>
#include "common.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint64_t g_last_nr;
static int64_t svc(armtr_cpu*, uint64_t nr, const uint64_t a[6], void*) { g_last_nr = nr; return (int64_t)(a[0] * 3 + nr); }
static uint8_t* G;
static void tramp(armtr_cpu* c, void* user) {
  armtr_regs r; armtr_regs_get(c, &r);
  r.x[0] = r.x[0] * 10 + r.x[1] + (uint64_t)user;
  armtr_regs_set(c, &r);
}
static void tramp_nest(armtr_cpu* c, void*) {
  armtr_regs r; armtr_regs_get(c, &r);
  uint64_t a[2] = {r.x[0], 5}, out = 0;
  CHECK(armtr_call(c, slot(G, F_ADD), a, 2, &out) == 0);
  // outer state must be intact after the nested call
  armtr_regs r2; armtr_regs_get(c, &r2);
  CHECK(r2.pc == r.pc && r2.sp == r.sp && r2.x[30] == r.x[30]);
  r2.x[0] = out + 1; armtr_regs_set(c, &r2);
}

static void tramp_nestfault(armtr_cpu* c, void*) {
  uint8_t* hole = (uint8_t*)mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint64_t a[1] = {(uint64_t)hole}, o;
  CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &o) != 0);
  armtr_regs r; armtr_regs_get(c, &r); r.x[0] = 77; armtr_regs_set(c, &r);
  munmap(hole, 4096);
}

static armtr_fault g_seen;
static uint64_t g_seen_pc;
static volatile int g_kicks;
static armtr_cpu* g_kick_cpu;
static int sig_hook(armtr_cpu* c, const armtr_fault* f, int64_t, uint64_t) {
  if (!f) { g_kicks++; return 0; }
  armtr_regs r; armtr_regs_get(c, &r);
  g_seen = *f; g_seen_pc = r.pc;
  r.pc += 4; r.x[0] = 42;   // "handler": skip the faulting load
  armtr_regs_set(c, &r);
  return 1;
}
static void on_alrm(int) { armtr_kick(g_kick_cpu); }

struct Th { uint8_t* g; uint64_t* cell; int n; };
static void* thr(void* p) {
  Th* t = (Th*)p;
  armtr_cpu* c = armtr_cpu_create(svc, 0);
  uint64_t a[2] = {(uint64_t)t->cell, (uint64_t)t->n}, r;
  CHECK(armtr_call(c, slot(t->g, F_ATOMIC), a, 2, &r) == 0);
  for (int i = 0; i < 2000; i++) { uint64_t b[2] = {(uint64_t)i, 1}; CHECK(armtr_call(c, slot(t->g, F_ADD), b, 2, &r) == 0 && r == (uint64_t)i + 2); }
  armtr_cpu_destroy(c);
  return 0;
}

int main() {
  CHECK(armtr_init() == 0);
  G = load_guest();
  armtr_cpu* c = armtr_cpu_create(svc, 0);
  CHECK(c);
  uint64_t r = 0;

  { uint64_t a[2] = {5, 7}; CHECK(armtr_call(c, slot(G, F_ADD), a, 2, &r) == 0 && r == 19); }
  { uint64_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}; CHECK(armtr_call(c, slot(G, F_SUM8), a, 8, &r) == 0 && r == 36); }
  { uint64_t a[2] = {221, 14}; CHECK(armtr_call(c, slot(G, F_SVC), a, 2, &r) == 0 && r == 14 * 3 + 221 && g_last_nr == 221); }

  { // trampoline with args and return value; callee-visible lr preserved by guest
    uint64_t t = (uint64_t)G + 0x8000;
    CHECK(armtr_trampoline_add(t, tramp, (void*)7) == 0);
    uint64_t a[3] = {t, 4, 5};
    CHECK(armtr_call(c, slot(G, F_TRAMP), a, 3, &r) == 0);
    CHECK(r == (4 * 10 + 5 + 7) + 1000);
  }
  { // nested guest call from inside a trampoline
    uint64_t t = (uint64_t)G + 0x8100;
    CHECK(armtr_trampoline_add(t, tramp_nest, 0) == 0);
    uint64_t a[2] = {t, 100};
    CHECK(armtr_call(c, slot(G, F_NEST), a, 2, &r) == 0);
    CHECK(r == 100 + 2 * 5 + 1);
  }
  { // NEON
    uint32_t A[4] = {1, 2, 3, 4}, B[4] = {10, 20, 30, 0xffffffffu}, O[4] = {0};
    uint64_t a[3] = {(uint64_t)A, (uint64_t)B, (uint64_t)O};
    CHECK(armtr_call(c, slot(G, F_NEON), a, 3, &r) == 0);
    CHECK(O[0] == 11 && O[1] == 22 && O[2] == 33 && O[3] == 3);
    armtr_regs rg; armtr_regs_get(c, &rg);
    uint32_t v2[4]; memcpy(v2, rg.v[2], 16);
    CHECK(v2[0] == 11 && v2[3] == 3);
  }
  { // registers: FP double, tpidr, fpcr, nzcv, v-regs set/get
    armtr_regs rg; armtr_regs_get(c, &rg);
    double d = 3.0; memset(rg.v[0], 0, 16); memcpy(rg.v[0], &d, 8);
    rg.tpidr_el0 = 0x123456780ULL; rg.pc = slot(G, F_FP); rg.x[30] = (uint64_t)G + 0xF000;
    armtr_regs_set(c, &rg);
    CHECK(armtr_run(c, (uint64_t)G + 0xF000) == 0);
    armtr_regs g2; armtr_regs_get(c, &g2);
    double o; memcpy(&o, g2.v[0], 8);
    CHECK(o == 9.0 && g2.tpidr_el0 == 0x123456780ULL);
    uint64_t a[1] = {0}; CHECK(armtr_call(c, slot(G, F_TLS), a, 0, &r) == 0 && r == 0x123456780ULL);
    g2.nzcv = 0xA0000000u; g2.fpcr = 0x400000; armtr_regs_set(c, &g2);
    armtr_regs g3; armtr_regs_get(c, &g3);
    CHECK(g3.nzcv == 0xA0000000u && g3.fpcr == 0x400000);
  }
  { // memory sharing with host buffers
    uint64_t* arr = (uint64_t*)mmap(0, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < 1000; i++) arr[i] = i;
    uint64_t a[3] = {(uint64_t)arr, 1000, (uint64_t)&arr[2000]};
    CHECK(armtr_call(c, slot(G, F_SUMARR), a, 3, &r) == 0 && r == 499500 && arr[2000] == 499500);
    uint64_t s[2] = {(uint64_t)&arr[3000], 0xdeadbeefcafeULL};
    CHECK(armtr_call(c, slot(G, F_STORE), s, 2, &r) == 0 && arr[3000] == 0xdeadbeefcafeULL);
  }
  { // faults: null, kernel half, host-unmapped page inside window; cpu reusable afterwards
    uint64_t a[1] = {0x10}, ad, pcf;
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) != 0);
    CHECK(armtr_fault_info(c, &ad, &pcf) == 1 && ad == 0x10 && pcf == slot(G, F_LOAD));
    printf("  err: %s\n", armtr_last_error(c));
    a[0] = 0xffff000000000000ULL;
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) != 0);
    uint8_t* hole = (uint8_t*)mmap(0, 8192, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    a[0] = (uint64_t)hole + 8;
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) != 0);
    CHECK(armtr_fault_info(c, &ad, &pcf) != 0 && ad == (uint64_t)hole + 8);
    printf("  err: %s\n", armtr_last_error(c));
    munmap(hole, 8192);
    uint64_t b[2] = {1, 1};
    CHECK(armtr_call(c, slot(G, F_ADD), b, 2, &r) == 0 && r == 3);  // still usable
    // stack guard: deep recursion-like overflow -> fault, not crash
    uint64_t sb; size_t ss; armtr_cpu_stack(c, &sb, &ss);
    a[0] = sb - 8;
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) != 0);
    CHECK(armtr_call(c, slot(G, F_ADD), b, 2, &r) == 0 && r == 3);
  }
  { // host-unmapped fault inside a nested call: nested call fails, outer guest code continues
    uint64_t t = (uint64_t)G + 0x8200;
    CHECK(armtr_trampoline_add(t, tramp_nestfault, 0) == 0);
    uint64_t a[2] = {t, 100};
    CHECK(armtr_call(c, slot(G, F_NEST), a, 2, &r) == 0 && r == 77);
  }
  { // signal hook: faults at the exact insn can be resumed; armtr_kick from a host signal while spinning
    armtr_set_signal_hook(sig_hook);
    uint8_t* hole = (uint8_t*)mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t a[1] = {(uint64_t)hole + 8};
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) == 0 && r == 42);
    CHECK(g_seen.signo == SIGSEGV && g_seen.code == SEGV_ACCERR && g_seen.addr == (uint64_t)hole + 8 &&
          g_seen_pc == slot(G, F_LOAD) && (g_seen.esr >> 26) == 0x24);
    munmap(hole, 4096);
    a[0] = 0x10;
    CHECK(armtr_call(c, slot(G, F_LOAD), a, 1, &r) == 0 && r == 42);
    CHECK(g_seen.signo == SIGSEGV && g_seen.code == SEGV_MAPERR && g_seen.addr == 0x10 && g_seen_pc == slot(G, F_LOAD));
    g_kick_cpu = c;
    signal(SIGALRM, on_alrm);
    struct itimerval it = {{0, 2000}, {0, 2000}}, off = {};
    setitimer(ITIMER_REAL, &it, nullptr);
    a[0] = 400000000;
    CHECK(armtr_call(c, slot(G, F_SPIN), a, 1, &r) == 0 && r == 0);
    setitimer(ITIMER_REAL, &off, nullptr);
    printf("  kicks while spinning: %d\n", g_kicks);
    CHECK(g_kicks > 0);
    armtr_set_signal_hook(nullptr);
  }
  { // two cpus, two threads, shared counter with ldaxr/stlxr
    uint64_t* cell = (uint64_t*)mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Th t1{G, cell, 100000}, t2{G, cell, 100000};
    pthread_t a, b;
    pthread_create(&a, 0, thr, &t1); pthread_create(&b, 0, thr, &t2);
    pthread_join(a, 0); pthread_join(b, 0);
    printf("  atomic cell=%llu (expect 200000)\n", (unsigned long long)*cell);
    if (getenv("ARMTR_UNPATCHED_UNICORN")) printf("  (unpatched Unicorn: cross-engine atomics not enforced)\n");
    else CHECK(*cell == 200000);
  }
  armtr_cpu_destroy(c);
  printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
  return fails != 0;
}
