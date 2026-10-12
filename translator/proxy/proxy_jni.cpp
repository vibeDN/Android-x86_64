// SPDX-License-Identifier: GPL-2.0-or-later
// Guest-visible JNIEnv / JavaVM proxy tables forwarding to the host ones.
//
// Design: the guest's JNIEnv* points to {functions, host_env} in guest-visible memory; `functions` is a
// shared 233-slot table of proxy stubs (layout/order generated from jni.h, checked against
// sizeof(JNINativeInterface)). Plain slots forward through the host env's own table (looked up per call,
// so checkjni-style tables work). Variadic JNI calls (CallXMethod, CallNonvirtualXMethod,
// CallStaticXMethod, NewObject) and their va_list (..V) form are NOT forwarded as such: GetMethodID /
// GetStaticMethodID are intercepted and the parameter descriptor is cached per jmethodID; the call is then
// rebuilt as a jvalue[] by reading AAPCS64 regs/stack (or the guest va_list struct) according to that
// descriptor and forwarded to the host ..A entry. varargs promotion rules apply (jfloat arrives as double).
// jmethodIDs not obtained through the proxy must be registered with armtr_proxy_jni_register_method.
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "internal.h"

using U = uint64_t;
namespace armtr_px {
U make_proxy(const char* name, const char* sig, void* fn, bool xlate, int tbl);
struct GuestObj { const void* functions; void* host; };
}
using namespace armtr_px;

namespace {
struct Ent { const char* name; char ret; const char* args; };
const Ent kEnt[] = {
#define JNI_RESERVED(n) {#n, 0, ""},
#define JNI_FN(n, r, a) {#n, r, a},
#include "jni_table.inc"
#undef JNI_RESERVED
#undef JNI_FN
};
constexpr int kN = sizeof(kEnt) / sizeof(kEnt[0]);
static_assert(kN == sizeof(JNINativeInterface) / sizeof(void*), "jni_table.inc out of sync with jni.h");
static_assert(sizeof(JNIInvokeInterface) / sizeof(void*) == 8, "JNIInvokeInterface changed");

std::mutex g_mu;
std::unordered_map<void*, std::string> g_methods;   // jmethodID -> param kinds (Z B C S I J F D L)
std::unordered_map<void*, GuestObj*> g_envs, g_vms;
void** g_env_tbl; void** g_vm_tbl;

std::string params_fixed(const char* d) {
  std::string o;
  if (!d || *d != '(') return o;
  for (d++; *d && *d != ')'; d++) {
    bool arr = false;
    while (*d == '[') { arr = true; d++; }
    if (*d == 'L') { while (*d && *d != ';') d++; o += 'L'; if (!*d) break; }
    else o += arr ? 'L' : *d;
  }
  return o;
}
void remember2(void* mid, const char* desc) {
  if (!mid) return;
  std::lock_guard<std::mutex> l(g_mu);
  g_methods[mid] = params_fixed(desc);
}

// ---- host-side wrappers (arg0 already translated to the host env/vm) -----------------------------------
jmethodID w_GetMethodID(JNIEnv* e, jclass c, const char* n, const char* s) {
  jmethodID m = e->GetMethodID(c, n, s); remember2(m, s); return m;
}
jmethodID w_GetStaticMethodID(JNIEnv* e, jclass c, const char* n, const char* s) {
  jmethodID m = e->GetStaticMethodID(c, n, s); remember2(m, s); return m;
}
GuestObj* guest_vm(void* host);
GuestObj* guest_env(void* host);
jint w_GetJavaVM(JNIEnv* e, JavaVM** out) {
  jint r = e->GetJavaVM(out);
  if (r == JNI_OK && out && *out) *out = (JavaVM*)guest_vm(*out);
  return r;
}
void fix_env_out(jint r, JNIEnv** penv) { if (r == JNI_OK && penv && *penv) *penv = (JNIEnv*)guest_env(*penv); }
jint w_Attach(JavaVM* v, JNIEnv** penv, void* a) { jint r = v->AttachCurrentThread(penv, a); fix_env_out(r, penv); return r; }
jint w_AttachDaemon(JavaVM* v, JNIEnv** penv, void* a) { jint r = v->AttachCurrentThreadAsDaemon(penv, a); fix_env_out(r, penv); return r; }
jint w_GetEnv(JavaVM* v, void** penv, jint ver) {
  jint r = v->GetEnv(penv, ver);
  if ((ver >> 16) == 1) fix_env_out(r, (JNIEnv**)penv);   // JNI_VERSION_1_x only (not JVMTI)
  return r;
}

// ---- variadic / va_list Call* ------------------------------------------------------------------------------
struct CallInfo { int a_idx; char ret; int nfixed; bool dots; };

void call_cb(armtr_cpu* cpu, void* user) {
  const CallInfo* ci = (const CallInfo*)user;
  CurGuard g(cpu);
  armtr_regs r; armtr_regs_get(cpu, &r);
  void* henv = ((GuestObj*)r.x[0])->host;
  U mid = r.x[ci->nfixed - 1];
  std::string d;
  { std::lock_guard<std::mutex> l(g_mu); auto it = g_methods.find((void*)mid); if (it != g_methods.end()) d = it->second;
    else fprintf(stderr, "armtr proxy: unknown jmethodID %p in JNI variadic call; assuming no args\n", (void*)mid); }
  std::vector<jvalue> jv(d.size() + 1);
  armtr_va va;
  if (ci->dots) armtr_va_from_regs(&va, &r, ci->nfixed, 0); else armtr_va_from_list(&va, r.x[ci->nfixed]);
  for (size_t i = 0; i < d.size(); i++) {
    jv[i].j = 0;
    switch (d[i]) {
      case 'Z': jv[i].z = (jboolean)armtr_va_int(&va); break;
      case 'B': jv[i].b = (jbyte)armtr_va_int(&va); break;
      case 'C': jv[i].c = (jchar)armtr_va_int(&va); break;
      case 'S': jv[i].s = (jshort)armtr_va_int(&va); break;
      case 'I': jv[i].i = (jint)armtr_va_int(&va); break;
      case 'J': jv[i].j = (jlong)armtr_va_int(&va); break;
      case 'F': jv[i].f = (jfloat)armtr_va_double(&va); break;
      case 'D': jv[i].d = armtr_va_double(&va); break;
      default:  jv[i].l = (jobject)armtr_va_int(&va); break;
    }
  }
  U ints[6] = {}, fp[8] = {}, stk[24] = {};
  int n = 0;
  ints[n++] = (U)henv;
  for (int k = 1; k < ci->nfixed - 1; k++) ints[n++] = r.x[k];
  ints[n++] = mid; ints[n++] = (U)jv.data();
  void* fn = (*(void***)henv)[ci->a_idx];
  store_ret(r, ci->ret, raw_call(fn, ci->ret, ints, fp, stk));
  armtr_regs_set(cpu, &r);
}

void build_tables() {
  if (g_env_tbl) return;
  static std::deque<CallInfo> infos;
  void** t = (void**)armtr_proxy_data_alloc(kN * sizeof(void*));
  for (int i = 0; i < kN; i++) {
    const Ent& e = kEnt[i];
    std::string nm = std::string("JNIEnv::") + e.name;
    if (!e.ret) continue;                                   // reserved
    std::string sig(1, e.ret); sig += e.args;
    bool dots = strchr(e.args, '.'), va = strchr(e.args, 'V');
    if (dots || va) {
      int nf = (int)(strchr(e.args, dots ? '.' : 'V') - e.args);
      infos.push_back(CallInfo{i + (dots ? 2 : 1), e.ret, nf, dots});
      t[i] = (void*)armtr_proxy_make_raw(nm.c_str(), call_cb, &infos.back());
      continue;
    }
    void* fn = nullptr; int tbl = i;
    if (!strcmp(e.name, "GetMethodID")) { fn = (void*)w_GetMethodID; tbl = -1; }
    else if (!strcmp(e.name, "GetStaticMethodID")) { fn = (void*)w_GetStaticMethodID; tbl = -1; }
    else if (!strcmp(e.name, "GetJavaVM")) { fn = (void*)w_GetJavaVM; tbl = -1; }
    t[i] = (void*)make_proxy(nm.c_str(), sig.c_str(), fn, true, tbl);
  }
  void** vt = (void**)armtr_proxy_data_alloc(8 * sizeof(void*));
  vt[3] = (void*)make_proxy("JavaVM::DestroyJavaVM", "ip", nullptr, true, 3);
  vt[4] = (void*)make_proxy("JavaVM::AttachCurrentThread", "ippp", (void*)w_Attach, true, -1);
  vt[5] = (void*)make_proxy("JavaVM::DetachCurrentThread", "ip", nullptr, true, 5);
  vt[6] = (void*)make_proxy("JavaVM::GetEnv", "ippi", (void*)w_GetEnv, true, -1);
  vt[7] = (void*)make_proxy("JavaVM::AttachCurrentThreadAsDaemon", "ippp", (void*)w_AttachDaemon, true, -1);
  g_vm_tbl = vt; g_env_tbl = t;
}

GuestObj* get(std::unordered_map<void*, GuestObj*>& m, void* host, void** tbl) {
  if (!host) return nullptr;
  { std::lock_guard<std::mutex> l(g_mu); auto it = m.find(host); if (it != m.end()) return it->second; }
  GuestObj* o = (GuestObj*)armtr_proxy_data_alloc(sizeof(GuestObj));
  if (!o) return nullptr;
  o->functions = tbl; o->host = host;
  std::lock_guard<std::mutex> l(g_mu);
  return m.emplace(host, o).first->second;
}
GuestObj* guest_env(void* host) { return get(g_envs, host, g_env_tbl); }
GuestObj* guest_vm(void* host) { return get(g_vms, host, g_vm_tbl); }
std::once_flag g_once;
}  // namespace

extern "C" {
uint64_t armtr_proxy_jnienv(void* host_jnienv) {
  std::call_once(g_once, build_tables);
  return (U)guest_env(host_jnienv);
}
uint64_t armtr_proxy_javavm(void* host_javavm) {
  std::call_once(g_once, build_tables);
  return (U)guest_vm(host_javavm);
}
int armtr_proxy_jni_index(const char* name) {
  for (int i = 0; i < kN; i++) if (kEnt[i].ret && !strcmp(kEnt[i].name, name)) return i;
  return -1;
}
void armtr_proxy_jni_register_method(void* mid, const char* desc) { remember2(mid, desc); }
}
