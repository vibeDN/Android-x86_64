// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone fake of the engine memory API so the loader links and is tested without the engine.
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <map>
#include "armtr/engine.h"
#include "fake_engine.h"

static std::map<uint64_t, FakeRegion> g_regions;   // addr -> region

static bool host_mapped(uint64_t a, size_t l) {
  unsigned char v[1];
  for (uint64_t p = a & ~4095ull; p < a + l; p += 4096) if (mincore((void*)p, 1, v) != 0) return false;
  return true;
}
extern "C" int armtr_mem_map(uint64_t addr, size_t len, int prot) {
  if ((addr | len) & 4095 || !len || !host_mapped(addr, len)) return -1;   // loader must have mmapped it
  for (auto& [a, r] : g_regions) if (addr < a + r.len && a < addr + len) return -1;   // overlap
  g_regions[addr] = {addr, len, prot};
  return 0;
}
extern "C" int armtr_mem_unmap(uint64_t addr, size_t len) {
  auto it = g_regions.find(addr);
  if (it == g_regions.end() || it->second.len != len) return -1;
  g_regions.erase(it);
  return 0;
}
extern "C" int armtr_mem_protect(uint64_t addr, size_t len, int prot) {
  auto it = g_regions.find(addr);
  if (it == g_regions.end()) return -1;
  it->second.prot = prot; (void)len;
  return 0;
}
size_t fake_region_count() { return g_regions.size(); }
bool fake_region_find(uint64_t addr, FakeRegion* out) {
  for (auto& [a, r] : g_regions) if (addr >= a && addr < a + r.len) { *out = r; return true; }
  return false;
}
