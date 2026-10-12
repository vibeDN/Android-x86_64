// SPDX-License-Identifier: GPL-2.0-or-later
// Lazy resolver for the host platform libraries (see hostlibs.h and gen_hostlibs.py for the table format).
#include "hostlibs.h"
#include <dlfcn.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <utility>
#include <stdio.h>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "internal.h"   // armtr_px::CurGuard (innermost-trampoline CPU for nested guest calls)
#include "proxy_ext.h"

namespace {
using U = uint64_t;

struct Ent { const char *name, *sig, *act, *cb; };
struct ItfDef { const char* name; const Ent* m; int n; };
struct IidDef { const char *name, *itf; };
#include "gen/hostlibs_tables.inc"

enum { L_LOG, L_Z, L_ANDROID, L_JNIGFX, L_EGL, L_GLES2, L_GLES1, L_SLES, L_N };
struct LibDef { const char* id; const char* hosts[3]; const Ent* tab; size_t n; };
#define TAB(x) kTab_##x, sizeof(kTab_##x) / sizeof(Ent)
const LibDef kLibs[L_N] = {
  {"liblog", {"liblog.so"}, TAB(LOG)},
  {"libz", {"libz.so"}, TAB(Z)},
  {"libandroid", {"libandroid.so", "libnativewindow.so"}, TAB(ANDROID)},
  {"libjnigraphics", {"libjnigraphics.so"}, TAB(JNIGFX)},
  {"libEGL", {"libEGL.so"}, TAB(EGL)},
  {"libGLESv2", {"libGLESv2.so", "libGLESv3.so"}, TAB(GLES2)},
  {"libGLESv1_CM", {"libGLESv1_CM.so"}, TAB(GLES1)},
  {"libOpenSLES", {"libOpenSLES.so"}, TAB(SLES)},
};

std::mutex g_mu;                       // guards everything below
std::string g_dir;
std::vector<void*> g_h[L_N];
bool g_opened[L_N];
std::unordered_map<std::string, U> g_cache;
std::deque<std::string> g_names;       // stable storage for proxy names

int lib_from_hint(const char* h) {
  if (!h || !*h) return -1;
  std::string s = h;
  size_t sl = s.rfind('/'); if (sl != std::string::npos) s = s.substr(sl + 1);
  for (int i = 0; i < L_N; i++) {
    if (s.compare(0, strlen(kLibs[i].id), kLibs[i].id) == 0) return i;
    for (const char* hn : kLibs[i].hosts) if (hn && s == hn) return i;
  }
  if (s == "libGLESv3.so" ) return L_GLES2;
  return -2;                           // a hint naming something else (libc, libm...): not ours
}

const Ent* find_ent(int l, const char* name) {
  const Ent* t = kLibs[l].tab; size_t n = kLibs[l].n;
  const Ent* e = std::lower_bound(t, t + n, name, [](const Ent& a, const char* b) { return strcmp(a.name, b) < 0; });
  return e != t + n && !strcmp(e->name, name) ? e : nullptr;
}

void* host_sym(int l, const char* name) {
  std::unique_lock<std::mutex> lk(g_mu);
  if (!g_opened[l]) {
    g_opened[l] = true;
    for (const char* hn : kLibs[l].hosts) {
      if (!hn) break;
      void* h = nullptr;
      if (!g_dir.empty()) h = dlopen((g_dir + "/" + hn).c_str(), RTLD_NOW | RTLD_LOCAL);
      if (!h) h = dlopen(hn, RTLD_NOW | RTLD_LOCAL);
      if (h) g_h[l].push_back(h);
    }
  }
  std::vector<void*> hs = g_h[l];
  lk.unlock();
  for (void* h : hs) if (void* s = dlsym(h, name)) return s;
  return nullptr;
}

// ---- host -> guest callback pool ---------------------------------------------------------------------
// A host function pointer cannot be created at run time without a JIT, so a fixed pool of host functions
// (each taking 8 integer args) is handed out, one per distinct (guest fn, shape).
struct CbInfo { U guest; char ret; int n; bool sl; };
constexpr int kPool = 96;
CbInfo g_cb[kPool];
int g_ncb;
U sl_wrapper_for_host(U host);

template <size_t N> U pool_fn(U a, U b, U c, U d, U e, U f, U g, U h) {
  CbInfo ci;
  { std::lock_guard<std::mutex> l(g_mu); ci = g_cb[N]; }
  U args[8] = {a, b, c, d, e, f, g, h};
  if (ci.sl) args[0] = sl_wrapper_for_host(args[0]);
  U rv = 0;
  armtr_proxy_call_guest(ci.guest, args, ci.n, &rv);
  return rv;
}

template <size_t... I> std::array<void*, sizeof...(I)> pool_table(std::index_sequence<I...>) {
  return {{(void*)&pool_fn<I>...}};
}
const std::array<void*, kPool>& pool() { static auto t = pool_table(std::make_index_sequence<kPool>()); return t; }

// Host function pointer that calls guest `fn` (0 -> 0). `n` int args (<= 8), `sl`: arg0 is a host SL interface.
U make_cb(U fn, char ret, int n, bool sl) {
  if (!fn) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  for (int i = 0; i < g_ncb; i++)
    if (g_cb[i].guest == fn && g_cb[i].ret == ret && g_cb[i].n == n && g_cb[i].sl == sl) return (U)pool()[i];
  if (g_ncb == kPool) return 0;       // ponytail: fixed pool; grow kPool if an app registers more distinct callbacks
  g_cb[g_ncb] = {fn, ret, n, sl};
  return (U)pool()[g_ncb++];
}

// ---- OpenSL ES wrappers ------------------------------------------------------------------------------
// A guest-visible interface is {vtable*, host itf*}; the vtable holds proxy stubs, each unwrapping arg0 and
// calling the slot of the HOST object's vtable (the host function pointers are never visible to the guest).
struct WrapInfo { U host; int itf; };
std::unordered_map<U, WrapInfo> g_wrap;       // guest wrapper -> info
std::unordered_map<U, U> g_byhost;            // host itf -> guest wrapper
std::vector<U> g_vtab(sizeof(kItfs) / sizeof(ItfDef), 0);
struct Iid { const IidDef* d; U host, guest, slot; };
std::vector<Iid> g_iids;
std::unordered_map<U, U> g_iid_g2h, g_iid_h2g;
std::once_flag g_iid_once;

struct Simple;
void simple_tramp(armtr_cpu*, void*);
void unsupported_tramp(armtr_cpu* cpu, void*) {
  armtr_regs r; armtr_regs_get(cpu, &r); r.x[0] = 12; armtr_regs_set(cpu, &r);   // SL_RESULT_FEATURE_UNSUPPORTED
}
Simple* new_simple(const Ent* e, void* host, int slot);

int itf_index(const char* n) {
  for (size_t i = 0; i < sizeof(kItfs) / sizeof(ItfDef); i++) if (!strcmp(kItfs[i].name, n)) return (int)i;
  return -1;
}

U ensure_vtable(int k) {                      // g_mu held
  if (g_vtab[k]) return g_vtab[k];
  const ItfDef& d = kItfs[k];
  U* t = (U*)armtr_proxy_data_alloc(8 * (size_t)d.n);
  if (!t) return 0;
  for (int j = 0; j < d.n; j++) {
    const Ent& e = d.m[j];
    t[j] = *e.sig ? armtr_proxy_make_raw(e.name, simple_tramp, new_simple(&e, nullptr, j))
                  : armtr_proxy_make_raw(e.name, unsupported_tramp, nullptr);
  }
  return g_vtab[k] = (U)t;
}

U wrap(U host, int k) {
  if (!host || k < 0) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_byhost.find(host);
  if (it != g_byhost.end() && g_wrap[it->second].itf == k) return it->second;
  U vt = ensure_vtable(k);
  U* w = (U*)armtr_proxy_data_alloc(16);   // ponytail: wrappers are never freed (few objects per app)
  if (!vt || !w) return 0;
  w[0] = vt; w[1] = host;
  g_wrap[(U)w] = {host, k};
  g_byhost[host] = (U)w;
  return (U)w;
}
U unwrap(U g) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_wrap.find(g);
  return it == g_wrap.end() ? g : it->second.host;     // unknown: pass through (NULL, or already host)
}
void forget(U host) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_byhost.find(host);
  if (it != g_byhost.end()) { g_wrap.erase(it->second); g_byhost.erase(it); }
}
U sl_wrapper_for_host(U host) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_byhost.find(host);
  return it == g_byhost.end() ? host : it->second;
}

void iid_init() {                           // needs the host lib; g_mu NOT held
  std::call_once(g_iid_once, [] {
  for (const IidDef& d : kIids) {
    void* sym = host_sym(L_SLES, d.name);
    U host = sym ? *(U*)sym : 0;
    if (!host) continue;
    void* copy = armtr_proxy_data_alloc(16);   // the guest cannot read host .rodata via a stable address
    void* slot = armtr_proxy_data_alloc(8);
    if (!copy || !slot) continue;
    memcpy(copy, (void*)host, 16);
    *(U*)slot = (U)copy;
    std::lock_guard<std::mutex> l(g_mu);
    g_iids.push_back({&d, host, (U)copy, (U)slot});
    g_iid_g2h[(U)copy] = host; g_iid_h2g[host] = (U)copy;
  }
  });
}
U iid_g2h(U g) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_iid_g2h.find(g);
  return it == g_iid_g2h.end() ? g : it->second;
}
U iid_h2g(U h) {
  if (!h) return 0;
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_iid_h2g.find(h);
  if (it != g_iid_h2g.end()) return it->second;
  void* copy = armtr_proxy_data_alloc(16);
  if (!copy) return 0;
  memcpy(copy, (void*)h, 16);
  g_iid_g2h[(U)copy] = h; g_iid_h2g[h] = (U)copy;
  return (U)copy;
}
int itf_for_host_iid(U h) {                  // by pointer, else by content
  std::lock_guard<std::mutex> l(g_mu);
  for (auto& i : g_iids)
    if (i.host == h || (h && !memcmp((void*)i.host, (void*)h, 16))) return i.d->itf ? itf_index(i.d->itf) : -1;
  return -1;
}

// ---- generic int-class trampoline --------------------------------------------------------------------
struct GuestObj { const void* functions; void* host; };   // guest JNIEnv layout, see proxy_core.cpp
struct CbSpec { int idx; bool sl; char ret; int n; };
struct Simple {
  const Ent* e; void* host; int slot; char ret; int nargs; std::vector<CbSpec> cbs;
};

Simple* new_simple(const Ent* e, void* host, int slot) {
  Simple* s = new Simple{e, host, slot, e->sig[0], 0, {}};
  const char* p = strchr(e->sig, '(');
  for (p = p ? p + 1 : ""; *p && *p != ')' && *p != '.'; p++) s->nargs++;
  for (const char* c = e->cb; *c;) {            // "<idx>:<S|->:<ret>(<args>)[;...]"
    CbSpec cs{};
    cs.idx = (int)strtol(c, (char**)&c, 10);
    cs.sl = c[1] == 'S'; cs.ret = c[3]; c += 5;
    while (*c && *c != ')') { cs.n++; c++; }
    s->cbs.push_back(cs);
    c += *c == ')'; c += *c == ';';
  }
  return s;
}

// z_stream (LP64: identical layout on arm64 and x86_64): zalloc at +64, zfree at +72.
void zstream_callbacks(U strm) {
  U* z = (U*)strm;
  if (!strm) return;
  z[8] = make_cb(z[8], 'p', 3, false);
  z[9] = make_cb(z[9], 'v', 2, false);
}

struct DsCopy { U loc, fmt; };
struct LocCopy { uint32_t type, a, b, c; U dev; };

void simple_tramp(armtr_cpu* cpu, void* user) {
  Simple* s = (Simple*)user;
  armtr_px::CurGuard guard(cpu);
  armtr_regs r; armtr_regs_get(cpu, &r);
  U a[16] = {};
  int n = std::min(s->nargs, 16);
  for (int i = 0; i < n; i++) a[i] = i < 8 ? r.x[i] : ((const U*)r.sp)[i - 8];
  const char* act = s->e->act;
  int na = (int)strlen(act);
  std::vector<U> iids;
  DsCopy ds[2]; LocCopy loc[2]; int nds = 0;
  armtr_host_va hv; armtr_va gva;
  U self_host = 0, rv = 0;
  void* fn = s->host;
  bool fail = false;
  for (int i = 0; i < na && i < n && !fail; i++) {
    switch (act[i]) {
      case 'E': if (a[i]) a[i] = (U)((GuestObj*)a[i])->host; break;
      case 'C':
        for (const CbSpec& c : s->cbs) if (c.idx == i) a[i] = make_cb(a[i], c.ret, c.n, c.sl);
        break;
      case 'S': case 'X': a[i] = unwrap(a[i]); if (i == 0) self_host = a[0]; break;
      case 'I': a[i] = iid_g2h(a[i]); break;
      case 'A':
        if (a[i] && i > 0) {
          size_t cnt = a[i - 1] & 0xffff;
          iids.resize(cnt);
          for (size_t k = 0; k < cnt; k++) iids[k] = iid_g2h(((const U*)a[i])[k]);
          a[i] = (U)iids.data();
        }
        break;
      case 'D':
        if (a[i] && nds < 2) {
          DsCopy& d = ds[nds]; LocCopy& lc = loc[nds]; nds++;
          d = *(const DsCopy*)a[i];
          uint32_t lt = d.loc ? *(const uint32_t*)d.loc : 0;
          if (lt == 4 || lt == 3) {          // OUTPUTMIX {type, SLObjectItf} / IODEVICE {type, kind, id, SLObjectItf}
            memcpy(&lc, (const void*)d.loc, lt == 4 ? 16 : 24);
            if (lt == 4) { U o = unwrap(*(const U*)&lc.b); memcpy(&lc.b, &o, 8); }
            else lc.dev = unwrap(lc.dev);
            d.loc = (U)&lc;
          }
          a[i] = (U)&d;
        }
        break;
      case 'V':
        armtr_va_from_list(&gva, a[i]);
        if (i == 0 || armtr_va_to_host_printf(&gva, (const char*)a[i - 1], &hv) != 0) { fail = true; break; }
        a[i] = (U)(void*)&hv.ap;
        break;
      case 'Z': zstream_callbacks(a[i]); break;
      default: break;
    }
  }
  if (fail) { rv = (U)-1; goto done; }
  if (s->slot >= 0) {
    if (!a[0]) { rv = 2; goto done; }       // SL_RESULT_PARAMETER_INVALID
    fn = (*(void***)a[0])[s->slot];
  }
  rv = ((U(*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U))fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                                                                    a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
  for (int i = 0; i < na && i < n; i++) {
    switch (act[i]) {
      case 'W': if ((uint32_t)rv == 0 && a[i]) *(U*)a[i] = wrap(*(U*)a[i], itf_index("SLObjectItf")); break;
      case 'G':
        if ((uint32_t)rv == 0 && a[i]) {
          int k = itf_for_host_iid(a[i - 1]);
          if (k < 0) { *(U*)a[i] = 0; rv = 12; }
          else *(U*)a[i] = wrap(*(U*)a[i], k);
        }
        break;
      case 'Q': if ((uint32_t)rv == 0 && a[i]) *(U*)a[i] = iid_h2g(*(U*)a[i]); break;
      case 'X': forget(self_host); break;
      default: break;
    }
  }
done:
  armtr_px::store_ret(r, s->ret, rv);
  armtr_regs_set(cpu, &r);
}

// eglGetProcAddress(name): guest asks for a GL/EGL entry point -> proxy stub, never the raw host pointer.
uint64_t resolve_in(int lib, const char* sym);
void egl_gpa_tramp(armtr_cpu* cpu, void*) {
  armtr_px::CurGuard guard(cpu);
  armtr_regs r; armtr_regs_get(cpu, &r);
  const char* name = (const char*)r.x[0];
  U res = 0;
  if (name) for (int l : {L_GLES2, L_EGL, L_GLES1}) if ((res = resolve_in(l, name))) break;
  r.x[0] = res;
  armtr_regs_set(cpu, &r);
}

uint64_t make_ent(int lib, const Ent& e, void* host) {
  std::string key;
  const char* nm;
  { std::lock_guard<std::mutex> l(g_mu); g_names.push_back(std::string(kLibs[lib].id) + "!" + e.name); nm = g_names.back().c_str(); }
  if (!strcmp(e.name, "eglGetProcAddress")) return armtr_proxy_make_raw(nm, egl_gpa_tramp, nullptr);
  if (!*e.act && !*e.cb) return armtr_proxy_make(nm, e.sig, host);
  return armtr_proxy_make_raw(nm, simple_tramp, new_simple(&e, host, -1));
}

uint64_t resolve_in(int lib, const char* sym) {
  std::string key = std::string(kLibs[lib].id) + "!" + sym;
  { std::lock_guard<std::mutex> l(g_mu); auto it = g_cache.find(key); if (it != g_cache.end()) return it->second; }
  if (lib == L_SLES && !strncmp(sym, "SL_IID_", 7)) {
    iid_init();
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& i : g_iids) if (!strcmp(i.d->name, sym)) return g_cache[key] = i.slot;
    return 0;
  }
  const Ent* e = find_ent(lib, sym);
  if (!e) return 0;
  void* host = host_sym(lib, sym);
  if (!host && (lib == L_GLES1 || lib == L_GLES2 || lib == L_EGL)) {   // extension entry points
    using Gpa = void* (*)(const char*);
    if (auto gpa = (Gpa)host_sym(L_EGL, "eglGetProcAddress")) host = gpa(sym);
  }
  if (!host && strcmp(e->name, "eglGetProcAddress")) return 0;
  uint64_t a = make_ent(lib, *e, host);
  if (a) { std::lock_guard<std::mutex> l(g_mu); g_cache[key] = a; }
  return a;
}
}  // namespace

extern "C" {
uint64_t armtr_hostlibs_resolve(const char* lib_hint, const char* symbol, void*) {
  if (!symbol) return 0;
  int l = lib_from_hint(lib_hint);
  if (l == -2) return 0;
  if (l >= 0) return resolve_in(l, symbol);
  for (int k : {L_LOG, L_Z, L_ANDROID, L_JNIGFX, L_EGL, L_GLES2, L_GLES1, L_SLES})
    if (uint64_t a = resolve_in(k, symbol)) return a;
  return 0;
}
uint64_t armtr_hostlibs_resolve2(const char* h, const char* s) { return armtr_hostlibs_resolve(h, s, nullptr); }
const char* armtr_hostlibs_sig(const char* lib_hint, const char* symbol) {
  if (!symbol) return nullptr;
  int l = lib_from_hint(lib_hint);
  if (l == -2) return nullptr;
  for (int k : {L_LOG, L_Z, L_ANDROID, L_JNIGFX, L_EGL, L_GLES2, L_GLES1, L_SLES}) {
    if (l >= 0 && k != l) continue;
    if (const Ent* e = find_ent(k, symbol)) return e->sig;
    if (k == L_SLES && !strncmp(symbol, "SL_IID_", 7))
      for (const IidDef& d : kIids) if (!strcmp(d.name, symbol)) return "data";
  }
  return nullptr;
}
void armtr_hostlibs_set_dir(const char* dir) { std::lock_guard<std::mutex> l(g_mu); g_dir = dir ? dir : ""; }
}
