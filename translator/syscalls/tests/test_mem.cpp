// SPDX-License-Identifier: GPL-2.0-or-later
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "t.h"
// Strong definitions of the engine hooks: record calls.
static int g_map, g_unmap, g_prot; static uint64_t g_la, g_ll; static int g_lp;
extern "C" int armtr_mem_map(uint64_t a, size_t l, int p) { g_map++; g_la = a; g_ll = l; g_lp = p; return 0; }
extern "C" int armtr_mem_unmap(uint64_t a, size_t l) { g_unmap++; g_la = a; g_ll = l; return 0; }
extern "C" int armtr_mem_protect(uint64_t a, size_t l, int p) { g_prot++; g_la = a; g_ll = l; g_lp = p; return 0; }
int main() {
  const uint64_t PG = 4096;
  int64_t a = sc(A64_mmap, 0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(a > 0 && (a & 4095) == 0);
  CHECK_EQ(g_map, 1); CHECK_EQ(g_la, a); CHECK_EQ(g_ll, 2 * PG); CHECK_EQ(g_lp, 3);
  *(volatile int*)a = 42;                          // identity: the returned address is usable by the host directly
  CHECK_EQ(*(volatile int*)a, 42);
  CHECK_EQ(sc(A64_mprotect, a, PG, PROT_READ | 0x10 /*PROT_BTI*/), 0);   // arm-only bit is stripped
  CHECK_EQ(g_prot, 1); CHECK_EQ(g_lp, 1);
  CHECK_EQ(sc(A64_mprotect, a, PG, PROT_READ | PROT_WRITE), 0);
  // MAP_32BIT (0x40) is not an aarch64 flag: must not restrict the address range
  int64_t b = sc(A64_mmap, 0, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | 0x40, -1, 0);
  CHECK(b > 0);
  CHECK(b > 0xffffffffll);
  CHECK_EQ(sc(A64_munmap, b, PG), 0); CHECK_EQ(g_unmap, 1);
  // MAP_FIXED over our own mapping
  int64_t c = sc(A64_mmap, a, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK_EQ(c, a); CHECK_EQ(*(volatile int*)a, 0);
  // mremap move
  int64_t m = sc(A64_mremap, a, 2 * PG, 4 * PG, MREMAP_MAYMOVE, 0);
  CHECK(m > 0);
  CHECK_EQ(g_la, m); CHECK_EQ(g_ll, 4 * PG);
  CHECK_EQ(sc(A64_madvise, m, 4 * PG, MADV_DONTNEED), 0);
  CHECK_EQ(sc(A64_mlock, m, PG), 0); CHECK_EQ(sc(A64_munlock, m, PG), 0);
  CHECK_EQ(sc(A64_msync, m, PG, MS_ASYNC), 0);
  uint8_t vec[4]; CHECK_EQ(sc(A64_mincore, m, 4 * PG, P(vec)), 0);
  CHECK_EQ(sc(A64_munmap, m, 4 * PG), 0);
  // errors
  CHECK_EQ(sc(A64_munmap, 1, PG), -EINVAL);
  CHECK_EQ(sc(A64_mmap, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), -EINVAL);
  CHECK_EQ(sc(A64_mmap, 0, PG, PROT_READ, MAP_PRIVATE, 9999, 0), -EBADF);
  int before = g_map;
  CHECK_EQ(sc(A64_mmap, 0, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), -EINVAL); CHECK_EQ(g_map, before);  // failures are not reported
  CHECK_EQ(sc(A64_mprotect, 1, PG, PROT_READ), -EINVAL);
  CHECK_EQ(sc(A64_brk, 0), 0);
  DONE();
}
