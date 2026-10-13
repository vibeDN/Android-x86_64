// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "armtr/engine.h"

enum { F_ADD, F_SUM8, F_SVC, F_TRAMP, F_NEON, F_SUMARR, F_LOAD, F_STORE, F_SPIN, F_ATOMIC, F_TLS, F_FP, F_NEST, F_STORE_CODE };

static inline double now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

// Loads guest.bin into fresh RWX host memory (guest VA == host VA); returns base. slot(N) = base + N*0x40.
static inline uint8_t* load_guest() {
  FILE* f = fopen(GUEST_BIN, "rb");
  if (!f) { perror(GUEST_BIN); exit(2); }
  uint8_t* m = (uint8_t*)mmap(0, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  size_t n = fread(m, 1, 1 << 16, f); fclose(f);
  if (!n) exit(2);
  armtr_mem_map((uint64_t)m, 1 << 20, 7);
  return m;
}
static inline uint64_t slot(uint8_t* b, int n) { return (uint64_t)b + n * 0x40; }
