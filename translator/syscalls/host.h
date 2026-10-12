// SPDX-License-Identifier: GPL-2.0-or-later
// Raw x86_64 Linux syscall: returns the kernel result unchanged (negative errno), never touches libc errno.
#pragma once
static inline long rawsc(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
  long ret = n;
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  __asm__ volatile("syscall" : "+a"(ret) : "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return ret;
}
