// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_engine.h"
#include <string.h>
#include <atomic>
#include <map>
#include <mutex>
namespace {
std::mutex mu;
struct Tr { armtr_trampoline_fn fn; void* user; };
std::map<uint64_t, Tr> tramps;
std::map<uint64_t, std::function<void(armtr_cpu*)>> gfns;
struct Rng { uint64_t len; int prot; };
std::map<uint64_t, Rng> ranges;
std::atomic<int> created, destroyed;
}
extern "C" {
int armtr_init(void) { return 0; }
armtr_cpu* armtr_cpu_create(armtr_svc_fn, void*) { created++; return new armtr_cpu(); }
void armtr_cpu_destroy(armtr_cpu* c) { destroyed++; delete c; }
int armtr_mem_map(uint64_t a, size_t l, int p) { std::lock_guard<std::mutex> g(mu); ranges[a] = {l, p}; return 0; }
int armtr_mem_unmap(uint64_t a, size_t) { std::lock_guard<std::mutex> g(mu); ranges.erase(a); return 0; }
int armtr_mem_protect(uint64_t, size_t, int) { return 0; }
void armtr_regs_get(armtr_cpu* c, armtr_regs* r) { *r = c->r; }
void armtr_regs_set(armtr_cpu* c, const armtr_regs* r) { c->r = *r; }
int armtr_trampoline_add(uint64_t a, armtr_trampoline_fn fn, void* u) { std::lock_guard<std::mutex> g(mu); tramps[a] = {fn, u}; return 0; }
int armtr_run(armtr_cpu*, uint64_t) { return 0; }
const char* armtr_last_error(armtr_cpu*) { return ""; }
int armtr_call(armtr_cpu* c, uint64_t entry, const uint64_t* args, int n, uint64_t* ret) {
  std::function<void(armtr_cpu*)> f;
  { std::lock_guard<std::mutex> g(mu); auto it = gfns.find(entry); if (it == gfns.end()) return -1; f = it->second; }
  for (int i = 0; i < n && i < 8; i++) c->r.x[i] = args[i];
  c->r.pc = entry;
  f(c);
  uint64_t rv = c->r.x[0];
  c->r.x[19] = 0xdead; c->r.x[30] = 0xbeef; c->r.pc = 0;   // a real call clobbers these
  if (ret) *ret = rv;
  return 0;
}
}
void fake_tramp(armtr_cpu* cpu, uint64_t addr) {
  Tr t;
  { std::lock_guard<std::mutex> g(mu); t = tramps.at(addr); }
  t.fn(cpu, t.user);
}
void fake_guest_fn(uint64_t e, std::function<void(armtr_cpu*)> f) { std::lock_guard<std::mutex> g(mu); gfns[e] = f; }
bool fake_mapped(uint64_t a, int prot) {
  std::lock_guard<std::mutex> g(mu);
  for (auto& kv : ranges) if (a >= kv.first && a < kv.first + kv.second.len) return (kv.second.prot & prot) == prot;
  return false;
}
int fake_cpus_created() { return created; }
int fake_cpus_destroyed() { return destroyed; }
