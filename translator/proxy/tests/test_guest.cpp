// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <thread>
#include "check.h"
#include "fake_engine.h"
#include "proxy_ext.h"

static uint64_t G2;
static uint64_t sp_outer, sp_inner;
static long host_cb(long x) {
  uint64_t arg = x, rv = 0;
  CHECK(armtr_proxy_call_guest(G2, &arg, 1, &rv) == 0);
  return (long)rv + 1;
}

int main() {
  uint64_t P = ARMTR_PROXY(host_cb, "l(l)", host_cb);
  G2 = 0x1000; uint64_t G1 = 0x2000;
  fake_guest_fn(G2, [](armtr_cpu* c) { sp_inner = c->r.sp; c->r.x[0] *= 2; c->r.x[19] = 0x77; });
  fake_guest_fn(G1, [&](armtr_cpu* c) {
    sp_outer = c->r.sp;
    c->r.x[19] = 111; c->r.x[30] = 222;
    uint64_t sp0 = c->r.sp;
    fake_tramp(c, P);   // guest calls the proxy with x0 = its arg
    CHECK(c->r.x[19] == 111 && c->r.x[30] == 222 && c->r.sp == sp0);   // nested call restored everything
  });
  uint64_t arg = 5, ret = 0;
  CHECK(armtr_proxy_call_guest(G1, &arg, 1, &ret) == 0);
  CHECK(ret == 11);
  CHECK(sp_outer && sp_inner && sp_inner < sp_outer && sp_inner % 16 == 0);
  CHECK(fake_mapped(sp_outer - 16, 3));    // guest stack is registered with the engine
  CHECK(fake_cpus_created() == 1);
  CHECK(armtr_proxy_current_cpu() != nullptr);

  // a second thread gets its own CPU, which is destroyed at thread exit
  std::thread([&] {
    uint64_t r2 = 0, a2 = 6;
    CHECK(armtr_proxy_call_guest(G1, &a2, 1, &r2) == 0);
    CHECK(r2 == 13);
  }).join();
  CHECK(fake_cpus_created() == 2 && fake_cpus_destroyed() == 1);
  puts("test_guest OK");
  return 0;
}
