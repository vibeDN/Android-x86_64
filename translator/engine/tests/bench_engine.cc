// SPDX-License-Identifier: GPL-2.0-or-later
#include "common.h"
static int64_t svc(armtr_cpu*, uint64_t, const uint64_t a[6], void*) { return a[0]; }
static void tramp(armtr_cpu* c, void*) { (void)c; }
int main() {
  armtr_init();
  uint8_t* G = load_guest();
  armtr_cpu* c = armtr_cpu_create(svc, 0);
  uint64_t r, a[3] = {1, 2, 3};
  int N = 200000;
  armtr_call(c, slot(G, F_ADD), a, 2, &r);
  double t = now();
  for (int i = 0; i < N; i++) armtr_call(c, slot(G, F_ADD), a, 2, &r);
  printf("armtr_call(add) overhead: %.0f ns/call\n", (now() - t) / N * 1e9);
  t = now();
  for (int i = 0; i < N; i++) armtr_call(c, slot(G, F_SVC), a, 2, &r);
  printf("armtr_call + 1 svc: %.0f ns/call\n", (now() - t) / N * 1e9);
  uint64_t tr = (uint64_t)G + 0x8000;
  armtr_trampoline_add(tr, tramp, 0);
  uint64_t b[3] = {tr, 1, 2};
  t = now();
  for (int i = 0; i < N; i++) armtr_call(c, slot(G, F_TRAMP), b, 3, &r);
  double tt = (now() - t) / N * 1e9;
  printf("armtr_call + 1 trampoline: %.0f ns/call\n", tt);
  // in-guest trampoline cost: loop of M calls would need another guest fn; derive from svc delta
  uint64_t it = 300000000ULL, s[1] = {it};
  t = now(); armtr_call(c, slot(G, F_SPIN), s, 1, &r); double d = now() - t;
  printf("spin loop: %.0f M guest insn/s\n", 2.0 * it / d / 1e6);
  return 0;
}
