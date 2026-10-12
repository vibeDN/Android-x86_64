// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "fake_engine.h"
#include "proxy_ext.h"

struct GuestVaList { void *stack, *gr_top, *vr_top; int32_t gr_offs, vr_offs; };

static int host_vsn(char* b, size_t n, const char* f, va_list ap) { return vsnprintf(b, n, f, ap); }

int main() {
  // A callee with 2 named int args and 1 named fp arg: 6 x regs and 7 v regs remain in the save areas.
  uint64_t gr[6] = {10, 11, 12, 13, 14, 15};
  uint8_t vr[7][16] = {};
  for (int i = 0; i < 7; i++) { double d = 0.5 + i; memcpy(vr[i], &d, 8); }
  uint64_t stack[4] = {100, 0, 0, 0};
  { double d = 77.25; memcpy(&stack[1], &d, 8); stack[2] = 200; }
  GuestVaList l = {stack, gr + 6, vr + 7, -48, -112};

  armtr_va v;
  armtr_va_from_list(&v, (uint64_t)&l);
  for (int i = 0; i < 6; i++) CHECK(armtr_va_int(&v) == (uint64_t)(10 + i));
  CHECK(armtr_va_int(&v) == 100);                       // regs exhausted -> stack
  for (int i = 0; i < 7; i++) CHECK(armtr_va_double(&v) == 0.5 + i);
  CHECK(armtr_va_double(&v) == 77.25);                  // fp exhausted -> shared stack, next slot
  CHECK(armtr_va_int(&v) == 200);
  CHECK(l.gr_offs == -48);                              // original untouched (we copy)

  // register mode: 1 named int
  armtr_regs r; memset(&r, 0, sizeof r);
  for (int i = 0; i < 8; i++) r.x[i] = i;
  r.sp = (uint64_t)stack;
  armtr_va_from_regs(&v, &r, 1, 0);
  for (int i = 1; i < 8; i++) CHECK(armtr_va_int(&v) == (uint64_t)i);
  CHECK(armtr_va_int(&v) == 100);

  // printf conversion: int, long, string, double, %*d, %%, %zu
  uint64_t g2[8] = {7, (uint64_t)-5, (uint64_t)"hi", 0, 6, 42, 9, 0};
  uint8_t v2[8][16] = {}; { double d = 2.25; memcpy(v2[0], &d, 8); }
  GuestVaList l2 = {stack, g2 + 8, v2 + 8, -64, -128};
  armtr_va_from_list(&v, (uint64_t)&l2);
  armtr_host_va hv;
  CHECK(armtr_va_to_host_printf(&v, "%d %ld %s %.2f %*d %% %zu", &hv) == 0);
  char out[128];
  // consumed: 7, -5, "hi", double(v0), width=0?? -> layout: ints 7,-5,hi,0(=width? no) ...
  // ints in order: %d=7 %ld=-5 %s=hi (double comes from v) %*d: width=0 -> g2[3]=0, value=6; %zu=42
  host_vsn(out, sizeof out, "%d %ld %s %.2f %*d %% %zu", hv.ap);
  CHECK(!strcmp(out, "7 -5 hi 2.25 6 % 42"));
  CHECK(armtr_va_to_host_printf(&v, "%Lf", &hv) == -1);
  puts("test_va OK");
  return 0;
}
