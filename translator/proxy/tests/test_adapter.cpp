// SPDX-License-Identifier: GPL-2.0-or-later
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "fake_engine.h"
#include "proxy_ext.h"

static int64_t f1(int a, int64_t b, void* c, int d) { return a * 1 + b * 10 + (int64_t)(intptr_t)c * 100 + d * 1000; }
static double f2(int a, double b, float c, int64_t d, double e, int f) { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6; }
static float f3(float a, float b) { return a * 10 + b; }
static int64_t f4(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                  int64_t a8, int64_t a9) {
  int64_t a[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9}; int64_t s = 0;
  for (int i = 0; i < 10; i++) s += a[i] * (i + 1);
  return s;
}
static double f5(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7, double a8,
                 double a9, double a10, double a11) {
  double a[] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11}; double s = 0;
  for (int i = 0; i < 12; i++) s += a[i] * (i + 1);
  return s;
}
static double f6(int64_t i0, int64_t i1, int64_t i2, int64_t i3, int64_t i4, int64_t i5, int64_t i6, int64_t i7,
                 double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8) {
  int64_t a[] = {i0, i1, i2, i3, i4, i5, i6, i7}; double b[] = {d0, d1, d2, d3, d4, d5, d6, d7, d8}; double s = 0;
  for (int i = 0; i < 8; i++) s += a[i] * (i + 1);
  for (int i = 0; i < 9; i++) s += b[i] * (100 * (i + 1));
  return s;
}
static int f7(int a) { return a == -1 ? -1 : a + 1; }
static int f8(uint8_t z, int8_t b, int16_t h, uint16_t c, uint32_t u) { return z * 1 + b * 10 + h * 100 + c * 1000 + (u >> 31); }
static uint8_t f9(int a) { return a != 0; }

static armtr_cpu* cpu;
alignas(16) static uint64_t gstack[64];
static armtr_regs R;
static void reset() { memset(&R, 0, sizeof R); memset(gstack, 0, sizeof gstack); R.sp = (uint64_t)gstack; }
static void callp(uint64_t a) { armtr_regs_set(cpu, &R); fake_tramp(cpu, a); armtr_regs_get(cpu, &R); }
static void setd(int n, double d) { memset(R.v[n], 0, 16); memcpy(R.v[n], &d, 8); }
static void setf(int n, float f) { memset(R.v[n], 0xAA, 16); memcpy(R.v[n], &f, 4); }
static double getd(int n) { double d; memcpy(&d, R.v[n], 8); return d; }
static float getf(int n) { float f; memcpy(&f, R.v[n], 4); return f; }

int main() {
  armtr_init();
  cpu = armtr_cpu_create(nullptr, nullptr);

  uint64_t a1 = ARMTR_PROXY(f1, "l(ilpi)", f1);
  CHECK(a1 && a1 % 4 == 0 && fake_mapped(a1, 5));
  CHECK(armtr_proxy_lookup("f1") == a1);
  CHECK(armtr_proxy_make("f1", "l(ilpi)", (void*)f1) == a1);   // idempotent
  CHECK(armtr_proxy_lookup("nope") == 0);
  CHECK(armtr_proxy_make("bad", "q(i)", (void*)f1) == 0);
  CHECK(armtr_proxy_make("bad2", "i(i...i)", (void*)f1) == 0);

  reset(); R.x[0] = 0xffffffff00000003ull; R.x[1] = 4; R.x[2] = 5; R.x[3] = 6; callp(a1);
  CHECK(R.x[0] == 3 + 40 + 500 + 6000);

  uint64_t p2 = armtr_proxy_make("f2good", "d(idfldi)", (void*)f2);
  CHECK(p2);
  reset(); R.x[0] = 1; setd(0, 2.5); setf(1, 1.5f); R.x[1] = 7; setd(2, 0.25); R.x[2] = 3; callp(p2);
  CHECK(fabs(getd(0) - (1 + 5.0 + 4.5 + 28 + 1.25 + 18)) < 1e-12);

  uint64_t p3 = armtr_proxy_make("f3", "f(ff)", (void*)f3);
  reset(); setf(0, 2.f); setf(1, 3.f); callp(p3);
  CHECK(getf(0) == 23.f);

  uint64_t p4 = armtr_proxy_make("f4", "l(llllllllll)", (void*)f4);
  CHECK(p4);
  reset(); for (int i = 0; i < 8; i++) R.x[i] = i + 1; gstack[0] = 9; gstack[1] = 10; callp(p4);
  CHECK(R.x[0] == 385);

  uint64_t p5 = armtr_proxy_make("f5", "d(dddddddddddd)", (void*)f5);
  reset(); for (int i = 0; i < 8; i++) setd(i, i + 1); for (int i = 0; i < 4; i++) { double d = 9 + i; memcpy(&gstack[i], &d, 8); } callp(p5);
  CHECK(getd(0) == 650);

  // 8 ints + 9 doubles: ints overflow the host regs (6) but not the guest's; the 9th double is on the guest stack
  uint64_t p6 = armtr_proxy_make("f6", "d(llllllllddddddddd)", (void*)f6);
  CHECK(p6);
  reset(); for (int i = 0; i < 8; i++) R.x[i] = i + 1; for (int i = 0; i < 8; i++) setd(i, i + 1); { double d = 9; memcpy(&gstack[0], &d, 8); } callp(p6);
  CHECK(getd(0) == 204 + 100 * 285);

  uint64_t p7 = armtr_proxy_make("f7", "i(i)", (void*)f7);
  reset(); R.x[0] = 0xdeadbeefffffffffull; callp(p7);
  CHECK(R.x[0] == 0xffffffffffffffffull);
  uint64_t p7u = armtr_proxy_make("f7u", "u(i)", (void*)f7);
  reset(); R.x[0] = 0xffffffffull; callp(p7u);
  CHECK(R.x[0] == 0xffffffffull);

  uint64_t p8 = armtr_proxy_make("f8", "i(zbhcu)", (void*)f8);
  reset(); R.x[0] = 0x101; R.x[1] = 0xf1; R.x[2] = 0xabcdffff; R.x[3] = 0x1ffff; R.x[4] = 0x80000000; callp(p8);
  CHECK((int32_t)R.x[0] == 1 - 150 - 100 + 65535000 + 1);
  uint64_t p9 = armtr_proxy_make("f9", "z(i)", (void*)f9);
  reset(); R.x[0] = 5; R.x[1] = 99; callp(p9); CHECK(R.x[0] == 1);

  // variadic host function: snprintf(char*, size_t, const char*, ...) with 7 int-class + 1 fp unnamed
  uint64_t ps = ARMTR_PROXY(snprintf, "i(plp...)", snprintf);
  char buf[128] = {};
  const char* fmt = "%d|%s|%.1f|%d|%d|%d|%d|%d";
  reset(); R.x[0] = (uint64_t)buf; R.x[1] = sizeof buf; R.x[2] = (uint64_t)fmt; R.x[3] = 42; R.x[4] = (uint64_t)"str";
  R.x[5] = 1; R.x[6] = 2; R.x[7] = 3; gstack[0] = 4; gstack[1] = 5; setd(0, 3.5); callp(ps);
  CHECK(!strcmp(buf, "42|str|3.5|1|2|3|4|5"));
  CHECK((int)R.x[0] == (int)strlen(buf));

  armtr_proxy_def defs[] = {ARMTR_PROXY_DEF(t_a, "i(i)", f7), ARMTR_PROXY_DEF(t_b, "v(i)", f7)};
  CHECK(armtr_proxy_make_table(defs, 2) == 2);
  uint64_t s1 = armtr_proxy_stub_alloc(), s2 = armtr_proxy_stub_alloc();
  CHECK(s1 && s2 == s1 + 4);
  puts("test_adapter OK");
  return 0;
}
