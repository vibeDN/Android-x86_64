// SPDX-License-Identifier: GPL-2.0-or-later
// armtr proxy: stub allocation, registry, AAPCS64 -> SysV adapter, host->guest calls.
#include <string.h>
#include <sys/mman.h>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include "internal.h"

using U = uint64_t;
namespace armtr_px {

// ---- guest-visible memory ---------------------------------------------------------------------------
static std::mutex g_mu;
static std::unordered_map<std::string, U> g_reg;
static U g_stub_cur, g_stub_end;
static char *g_data_cur, *g_data_end;
static constexpr size_t kStubChunk = 64 << 10, kDataChunk = 256 << 10;

static void* map_guest(size_t len, int host_prot, int guest_prot, bool fill_ret) {
  void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) return nullptr;
  if (fill_ret) for (size_t i = 0; i < len / 4; i++) ((uint32_t*)m)[i] = 0xD65F03C0u;   // ret
  if (host_prot != (PROT_READ | PROT_WRITE)) mprotect(m, len, host_prot);
  if (armtr_mem_map((U)m, len, guest_prot) != 0) { munmap(m, len); return nullptr; }
  return m;
}

}  // namespace armtr_px
using namespace armtr_px;

extern "C" uint64_t armtr_proxy_stub_alloc(void) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_stub_cur == g_stub_end) {
    // Host-writable: armtr_trampoline_add writes its 8-byte `svc; ret` stub here (the engine ignores host prot).
    void* m = map_guest(kStubChunk, PROT_READ | PROT_WRITE, 7, true);
    if (!m) return 0;
    g_stub_cur = (U)m; g_stub_end = g_stub_cur + kStubChunk;
  }
  U a = g_stub_cur; g_stub_cur += 8; return a;
}

extern "C" void* armtr_proxy_data_alloc(size_t size) {
  std::lock_guard<std::mutex> l(g_mu);
  size = (size + 15) & ~(size_t)15;
  if (size > kDataChunk) return map_guest((size + 4095) & ~(size_t)4095, PROT_READ | PROT_WRITE, 3, false);
  if ((size_t)(g_data_end - g_data_cur) < size) {
    void* m = map_guest(kDataChunk, PROT_READ | PROT_WRITE, 3, false);
    if (!m) return nullptr;
    g_data_cur = (char*)m; g_data_end = g_data_cur + kDataChunk;
  }
  void* p = g_data_cur; g_data_cur += size; return p;
}

// ---- host call core ---------------------------------------------------------------------------------
namespace armtr_px {
template <class R>
static R rc(void* fn, const U* i, const double* d, const U* s) {
  // Non-variadic callees ignore the extra regs/stack; variadic callees get al=8 and the full set.
  return ((R(*)(U, U, U, U, U, U, ...))fn)(i[0], i[1], i[2], i[3], i[4], i[5], d[0], d[1], d[2], d[3], d[4],
      d[5], d[6], d[7], s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13],
      s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23]);
}
uint64_t raw_call(void* fn, char ret, const U* ints, const U* fp, const U* stk) {
  double d[8]; memcpy(d, fp, sizeof d);
  if (ret == 'f') { float f = rc<float>(fn, ints, d, stk); U b = 0; memcpy(&b, &f, 4); return b; }
  if (ret == 'd') { double x = rc<double>(fn, ints, d, stk); U b; memcpy(&b, &x, 8); return b; }
  return rc<U>(fn, ints, d, stk);
}
U norm(char c, U v) {
  switch (c) {
    case 'i': return (U)(int64_t)(int32_t)v;
    case 'u': return (uint32_t)v;
    case 'z': return (uint8_t)v;
    case 'b': return (U)(int64_t)(int8_t)v;
    case 'h': return (U)(int64_t)(int16_t)v;
    case 'c': return (uint16_t)v;
    default: return v;
  }
}
void store_ret(armtr_regs& r, char ret, U bits) {
  if (ret == 'v') return;
  if (ret == 'f' || ret == 'd') { memset(r.v[0], 0, 16); memcpy(r.v[0], &bits, ret == 'f' ? 4 : 8); return; }
  r.x[0] = norm(ret, bits);
}

// ---- current / thread-local CPU ---------------------------------------------------------------------
static thread_local armtr_cpu* t_cur;
CurGuard::CurGuard(armtr_cpu* c) : prev(t_cur) { t_cur = c; }
CurGuard::~CurGuard() { t_cur = prev; }

static armtr_svc_fn g_svc; static void* g_svc_user;
static void (*g_init)(armtr_cpu*, void*); static void* g_init_user;
static constexpr size_t kStack = 1 << 20;

struct Tls {
  armtr_cpu* cpu = nullptr; void* stk = nullptr;
  ~Tls() {
    if (cpu) armtr_cpu_destroy(cpu);
    if (stk) { armtr_mem_unmap((U)stk, kStack); munmap(stk, kStack); }
  }
};
static thread_local Tls t_tls;

static armtr_cpu* own_cpu() {
  if (t_tls.cpu) return t_tls.cpu;
  void* s = mmap(nullptr, kStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (s == MAP_FAILED) return nullptr;
  if (armtr_mem_map((U)s, kStack, 3) != 0) { munmap(s, kStack); return nullptr; }
  armtr_cpu* c = armtr_cpu_create(g_svc, g_svc_user);
  if (!c) { armtr_mem_unmap((U)s, kStack); munmap(s, kStack); return nullptr; }
  armtr_regs r; armtr_regs_get(c, &r); r.sp = (U)s + kStack - 64; armtr_regs_set(c, &r);
  if (g_init) g_init(c, g_init_user);
  t_tls.cpu = c; t_tls.stk = s;
  return c;
}

// ---- signature adapter ------------------------------------------------------------------------------
struct Sig { char ret = 'v'; uint8_t n = 0; bool var = false; char a[24]; };
struct GuestObj { const void* functions; void* host; };   // guest JNIEnv / JavaVM layout
struct Proxy { Sig sig; void* fn; bool xlate; int tbl; };

static bool parse_sig(const char* s, Sig& o) {
  if (!s || !*s || !strchr("vulpfdzbhci", *s)) return false;
  o.ret = *s++;
  if (*s == '(') s++;
  unsigned ni = 0, nf = 0;
  while (*s && *s != ')') {
    if (!strncmp(s, "...", 3)) { o.var = true; s += 3; continue; }
    if (o.var || o.n >= 24 || !strchr("iulpfdzbhc", *s)) return false;
    (*s == 'f' || *s == 'd') ? nf++ : ni++;
    o.a[o.n++] = *s++;
  }
  if (*s == ')') s++;
  return *s == 0 && (ni > 6 ? ni - 6 : 0) + (nf > 8 ? nf - 8 : 0) <= 14;
}

static void generic_cb(armtr_cpu* cpu, void* user) {
  const Proxy* p = (const Proxy*)user;
  CurGuard g(cpu);
  armtr_regs r; armtr_regs_get(cpu, &r);
  const U* gs = (const U*)r.sp;
  U ints[6] = {}, fp[8] = {}, stk[24] = {};
  unsigned gi = 0, gf = 0, gsx = 0, hi = 0, hf = 0, hs = 0;
  auto put = [&](bool isfp, U v) {
    if (isfp) { if (hf < 8) fp[hf++] = v; else stk[hs++] = v; }
    else      { if (hi < 6) ints[hi++] = v; else stk[hs++] = v; }
  };
  for (unsigned k = 0; k < p->sig.n; k++) {
    char c = p->sig.a[k];
    bool isfp = c == 'f' || c == 'd';
    U v;
    if (isfp) { if (gf < 8) memcpy(&v, r.v[gf++], 8); else v = gs[gsx++]; if (c == 'f') v = (uint32_t)v; }
    else      { v = gi < 8 ? r.x[gi++] : gs[gsx++]; v = norm(c, v); }
    put(isfp, v);
  }
  if (p->sig.var) {   // forward every remaining register and 8 stack slots, see proxy_ext.h
    while (gi < 8) put(false, r.x[gi++]);
    while (gf < 8) { U v; memcpy(&v, r.v[gf++], 8); put(true, v); }
    for (unsigned k = 0; k < 8; k++) stk[hs++] = gs[gsx + k];
  }
  void* fn = p->fn;
  if (p->xlate) {
    ints[0] = (U)((GuestObj*)ints[0])->host;
    if (p->tbl >= 0) fn = (*(void***)ints[0])[p->tbl];
  }
  store_ret(r, p->sig.ret, raw_call(fn, p->sig.ret, ints, fp, stk));
  armtr_regs_set(cpu, &r);
}

// Used by proxy_jni.cpp: a sig-adapted proxy that translates arg0 (guest JNIEnv/JavaVM -> host) and,
// if tbl >= 0, calls slot `tbl` of the host function table instead of `fn`.
U make_proxy(const char* name, const char* sig, void* fn, bool xlate, int tbl) {
  Sig s;
  if (!parse_sig(sig, s) || (xlate && (s.n == 0 || s.a[0] != 'p'))) return 0;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (name) { auto it = g_reg.find(name); if (it != g_reg.end()) return it->second; }
  }
  static std::deque<Proxy> store;   // stable addresses
  Proxy* p;
  { std::lock_guard<std::mutex> l(g_mu); store.push_back(Proxy{s, fn, xlate, tbl}); p = &store.back(); }
  return armtr_proxy_make_raw(name, generic_cb, p);
}
}  // namespace armtr_px

extern "C" {
uint64_t armtr_proxy_make_raw(const char* name, armtr_trampoline_fn fn, void* user) {
  uint64_t a = armtr_proxy_stub_alloc();
  if (!a || armtr_trampoline_add(a, fn, user) != 0) return 0;
  if (name) { std::lock_guard<std::mutex> l(g_mu); g_reg[name] = a; }
  return a;
}
uint64_t armtr_proxy_make(const char* name, const char* sig, void* host_fn) {
  return host_fn ? make_proxy(name, sig, host_fn, false, -1) : 0;
}
uint64_t armtr_proxy_lookup(const char* name) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_reg.find(name ? name : "");
  return it == g_reg.end() ? 0 : it->second;
}
int armtr_proxy_make_table(const armtr_proxy_def* d, int n) {
  int ok = 0;
  for (int i = 0; i < n; i++) ok += armtr_proxy_make(d[i].name, d[i].sig, d[i].fn) != 0;
  return ok;
}
void armtr_proxy_config(armtr_svc_fn svc, void* su, void (*init)(armtr_cpu*, void*), void* iu) {
  g_svc = svc; g_svc_user = su; g_init = init; g_init_user = iu;
}
armtr_cpu* armtr_proxy_current_cpu(void) { return t_cur ? t_cur : t_tls.cpu; }

// Re-entrant: inside a trampoline t_cur is the running CPU; the nested call runs on its stack below the
// current sp and all registers are restored afterwards.
int armtr_proxy_call_guest(uint64_t entry, const uint64_t* args, int nargs, uint64_t* ret) {
  armtr_cpu* cpu = t_cur ? t_cur : own_cpu();
  if (!cpu) return -1;
  armtr_regs saved; armtr_regs_get(cpu, &saved);
  armtr_regs r = saved;
  r.sp = (saved.sp - 256) & ~(U)15;   // AArch64 has no red zone; 256 keeps a margin for frame records
  armtr_regs_set(cpu, &r);
  uint64_t rv = 0;
  int rc2 = armtr_call(cpu, entry, args, nargs, &rv);
  armtr_regs_set(cpu, &saved);
  if (ret) *ret = rv;
  return rc2;
}
}
