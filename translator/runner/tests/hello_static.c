// SPDX-License-Identifier: GPL-2.0-or-later
// Freestanding static aarch64 test: raw syscalls only. Prints argc/argv[0], a few auxv values, exits with 42.
typedef unsigned long u64;
static long sc3(long n, long a, long b, long c) {
  register long x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
  __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}
static u64 len(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { sc3(64, 1, (long)s, (long)len(s)); }
static void hex(u64 v) {
  char b[19] = "0x................"; b[18] = 0;
  for (int i = 0; i < 16; i++) b[17 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
  out(b);
}
void cmain(u64* sp) {
  u64 argc = sp[0]; char** argv = (char**)(sp + 1);
  out("hello static argc="); hex(argc); out(" argv0="); out(argv[0]); out("\n");
  u64* e = (u64*)(argv + argc + 1);
  while (*e) e++;
  e++;
  for (; e[0]; e += 2) {
    if (e[0] == 6) { out("pagesz="); hex(e[1]); out("\n"); }
    if (e[0] == 16) { out("hwcap!=0:"); out(e[1] ? "yes" : "no"); out("\n"); }
    if (e[0] == 7) { out("at_base="); hex(e[1]); out("\n"); }
    if (e[0] == 23) { out("secure="); hex(e[1]); out("\n"); }
  }
  // NEON + atomics + mrs tpidr_el0 sanity
  u64 t = 0x1234;
  __asm__ volatile("msr tpidr_el0, %0\n mrs %0, tpidr_el0" : "+r"(t));
  out("tpidr="); hex(t); out("\n");
  u64 tcr; __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr)); out("tcr="); hex(tcr); out("\n");
  volatile u64 cell = 1;
  u64 tagged = (u64)&cell | (0xb4ul << 56);
  *(volatile u64*)tagged = 77;   // top-byte-ignore store
  out("tbi="); hex(cell); out("\n");
  sc3(94, 42, 0, 0);
}
__asm__(".globl _start\n_start:\n mov x0, sp\n and sp, x0, #-16\n bl cmain\n");
