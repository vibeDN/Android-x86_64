// SPDX-License-Identifier: GPL-2.0-or-later
// Tiny test helpers: call armtr_syscall with aarch64 numbers.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "armtr_syscalls_ext.h"
#include "a64_nr.h"
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { printf("FAIL %s:%d %s=%lld want %lld\n", __FILE__, __LINE__, #a, _a, _b); g_fail++; } } while (0)
static inline int64_t sc(uint64_t nr, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0, uint64_t f = 0) {
  uint64_t args[6] = {a, b, c, d, e, f};
  return armtr_syscall(nullptr, nr, args, nullptr);
}
#define P(x) ((uint64_t)(uintptr_t)(x))
#define DONE() do { printf(g_fail ? "FAILED (%d)\n" : "ok\n", g_fail); return g_fail ? 1 : 0; } while (0)
// aarch64 O_* values (arch/arm64/include/uapi/asm/fcntl.h)
enum { A_O_DIRECTORY = 040000, A_O_NOFOLLOW = 0100000, A_O_DIRECT = 0200000, A_O_LARGEFILE = 0400000 };
