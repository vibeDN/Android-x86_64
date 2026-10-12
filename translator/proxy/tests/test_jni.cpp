// SPDX-License-Identifier: GPL-2.0-or-later
#include <jni.h>
#include <math.h>
#include <set>
#include <stdarg.h>
#include <string.h>
#include "check.h"
#include "fake_engine.h"
#include "proxy_ext.h"

// ---- mock host JNIEnv/JavaVM recording calls ----
static JNINativeInterface T;
static JNIInvokeInterface VT;
static _JNIEnv HENV{&T};
static _JavaVM HVM{&VT};
static JNIEnv* const henv = &HENV;
static const void* last_env; static const jvalue* last_jv; static jvalue copy_jv[16];
static jobject last_obj; static jclass last_cls; static jmethodID last_mid; static const char* last_str;
static int host_calls;

static jint m_GetVersion(JNIEnv* e) { CHECK(e == henv); return JNI_VERSION_1_6; }
static jclass m_FindClass(JNIEnv* e, const char* n) { CHECK(e == henv); last_str = n; return (jclass)0x1234; }
static jmethodID m_GetMethodID(JNIEnv* e, jclass c, const char* n, const char* s) {
  CHECK(e == henv); last_cls = c; last_str = s; (void)n; return (jmethodID)(0x5000 + strlen(s));
}
static void rec(JNIEnv* e, jobject o, jclass c, jmethodID m, const jvalue* jv, int n) {
  CHECK(e == henv); last_obj = o; last_cls = c; last_mid = m; last_jv = jv; memcpy(copy_jv, jv, n * sizeof(jvalue)); host_calls++;
}
static jdouble m_CallDoubleMethodA(JNIEnv* e, jobject o, jmethodID m, const jvalue* jv) { rec(e, o, 0, m, jv, 7); return 6.5; }
static jint m_CallIntMethodA(JNIEnv* e, jobject o, jmethodID m, const jvalue* jv) { rec(e, o, 0, m, jv, 8); return -9; }
static jint m_CallNonvirtualIntMethodA(JNIEnv* e, jobject o, jclass c, jmethodID m, const jvalue* jv) { rec(e, o, c, m, jv, 2); return 31; }
static void m_CallStaticVoidMethodA(JNIEnv* e, jclass c, jmethodID m, const jvalue* jv) { rec(e, 0, c, m, jv, 3); }
static jfloat m_CallStaticFloatMethodA(JNIEnv* e, jclass c, jmethodID m, const jvalue* jv) { rec(e, 0, c, m, jv, 1); return 1.25f; }
static jobject m_NewObjectA(JNIEnv* e, jclass c, jmethodID m, const jvalue* jv) { rec(e, 0, c, m, jv, 2); return (jobject)0x777; }
static jboolean m_CallBooleanMethodA(JNIEnv* e, jobject o, jmethodID m, const jvalue* jv) { rec(e, o, 0, m, jv, 1); return (jboolean)0xff; }
static void m_SetIntField(JNIEnv* e, jobject o, jfieldID f, jint v) { CHECK(e == henv); last_obj = o; last_mid = (jmethodID)f; copy_jv[0].i = v; }
static const char* m_GetStringUTFChars(JNIEnv*, jstring, jboolean* c) { if (c) *c = JNI_TRUE; return "utf"; }
static jint m_GetJavaVM(JNIEnv*, JavaVM** o) { *o = &HVM; return JNI_OK; }
static jlong m_GetDirectBufferCapacity(JNIEnv*, jobject o) { return (jlong)(intptr_t)o * 2; }
static void m_ReleaseIntArrayElements(JNIEnv*, jintArray, jint* p, jint mode) { copy_jv[0].l = (jobject)p; copy_jv[1].i = mode; }
static jint v_GetEnv(JavaVM* v, void** out, jint ver) { CHECK(v == &HVM); if (ver != JNI_VERSION_1_6) return JNI_EVERSION; *out = henv; return JNI_OK; }
static jint v_Attach(JavaVM* v, JNIEnv** out, void* a) { CHECK(v == &HVM); last_str = (const char*)a; *out = henv; return JNI_OK; }
static jint v_Detach(JavaVM* v) { CHECK(v == &HVM); return 5; }

static armtr_cpu* cpu; static armtr_regs R; alignas(16) static uint64_t gstack[64];
static uint64_t genv; static void** gtbl;
static void reset() { memset(&R, 0, sizeof R); memset(gstack, 0, sizeof gstack); R.sp = (uint64_t)gstack; R.x[0] = genv; }
static void call(const char* name) {
  int i = armtr_proxy_jni_index(name); CHECK(i >= 0);
  armtr_regs_set(cpu, &R); fake_tramp(cpu, (uint64_t)gtbl[i]); armtr_regs_get(cpu, &R);
}
static void setd(int n, double d) { memset(R.v[n], 0, 16); memcpy(R.v[n], &d, 8); }
static double getd(int n) { double d; memcpy(&d, R.v[n], 8); return d; }
static uint64_t getmid(const char* fn, const char* d) { reset(); R.x[1] = 1; R.x[2] = (uint64_t)"m"; R.x[3] = (uint64_t)d; call(fn); return R.x[0]; }
struct GuestVaList { void *stack, *gr_top, *vr_top; int32_t gr_offs, vr_offs; };

int main() {
  T.GetVersion = m_GetVersion; T.FindClass = m_FindClass; T.GetMethodID = m_GetMethodID; T.GetStaticMethodID = m_GetMethodID;
  T.CallDoubleMethodA = m_CallDoubleMethodA; T.CallIntMethodA = m_CallIntMethodA; T.CallNonvirtualIntMethodA = m_CallNonvirtualIntMethodA;
  T.CallStaticVoidMethodA = m_CallStaticVoidMethodA; T.CallStaticFloatMethodA = m_CallStaticFloatMethodA; T.NewObjectA = m_NewObjectA;
  T.CallBooleanMethodA = m_CallBooleanMethodA; T.SetIntField = m_SetIntField; T.GetStringUTFChars = m_GetStringUTFChars;
  T.GetJavaVM = m_GetJavaVM; T.GetDirectBufferCapacity = m_GetDirectBufferCapacity; T.ReleaseIntArrayElements = m_ReleaseIntArrayElements;
  VT.GetEnv = v_GetEnv; VT.AttachCurrentThread = v_Attach; VT.DetachCurrentThread = v_Detach;

  cpu = armtr_cpu_create(nullptr, nullptr);
  genv = armtr_proxy_jnienv(henv);
  CHECK(genv && genv == armtr_proxy_jnienv(henv));
  CHECK(fake_mapped(genv, 3));
  gtbl = *(void***)genv;
  CHECK(*(void**)(genv + 8) == henv);
  // table shape: 233 slots, 4 reserved NULL, the rest unique executable stubs
  std::set<void*> seen;
  for (int i = 0; i < 233; i++) {
    if (i < 4) CHECK(!gtbl[i]);
    else { CHECK(gtbl[i] && fake_mapped((uint64_t)gtbl[i], 5) && seen.insert(gtbl[i]).second); }
  }
  CHECK(armtr_proxy_jni_index("GetVersion") == 4 && armtr_proxy_jni_index("GetObjectRefType") == 232);
  CHECK(armtr_proxy_jni_index("reserved0") == -1);

  reset(); call("GetVersion"); CHECK((int32_t)R.x[0] == JNI_VERSION_1_6);
  reset(); R.x[1] = (uint64_t)"java/lang/String"; call("FindClass"); CHECK(R.x[0] == 0x1234 && !strcmp(last_str, "java/lang/String"));
  reset(); R.x[1] = 0xbad; R.x[2] = 0xf00; R.x[3] = 0xabc00000007ull; call("SetIntField");
  CHECK(last_obj == (jobject)0xbad && last_mid == (jmethodID)0xf00 && copy_jv[0].i == 7);
  reset(); R.x[1] = 0; R.x[2] = 0xabc; R.x[3] = 0xffffffff0000000cull; call("ReleaseIntArrayElements");
  CHECK(copy_jv[0].l == (jobject)0xabc && copy_jv[1].i == 12);
  reset(); R.x[1] = 21; call("GetDirectBufferCapacity"); CHECK(R.x[0] == 42);
  reset(); R.x[1] = 0; call("GetStringUTFChars"); CHECK(!strcmp((char*)R.x[0], "utf"));

  // GetJavaVM returns the guest JavaVM
  uint64_t out = 0; reset(); R.x[1] = (uint64_t)&out; call("GetJavaVM");
  uint64_t gvm = armtr_proxy_javavm(&HVM);
  CHECK(R.x[0] == 0 && out == gvm && out == armtr_proxy_javavm(&HVM));

  // ---- JavaVM ----
  void** vtbl = *(void***)gvm;
  CHECK(!vtbl[0] && vtbl[3] && vtbl[7]);
  uint64_t penv = 0;
  reset(); R.x[0] = gvm; R.x[1] = (uint64_t)&penv; R.x[2] = JNI_VERSION_1_6; armtr_regs_set(cpu, &R); fake_tramp(cpu, (uint64_t)vtbl[6]); armtr_regs_get(cpu, &R);
  CHECK(R.x[0] == 0 && penv == genv);
  reset(); R.x[0] = gvm; R.x[1] = (uint64_t)&penv; R.x[2] = 0x30001; penv = 1; armtr_regs_set(cpu, &R); fake_tramp(cpu, (uint64_t)vtbl[6]); armtr_regs_get(cpu, &R);
  CHECK((int32_t)R.x[0] == JNI_EVERSION && penv == 1);
  penv = 0; reset(); R.x[0] = gvm; R.x[1] = (uint64_t)&penv; R.x[2] = (uint64_t)"args"; armtr_regs_set(cpu, &R); fake_tramp(cpu, (uint64_t)vtbl[4]); armtr_regs_get(cpu, &R);
  CHECK(R.x[0] == 0 && penv == genv && !strcmp(last_str, "args"));
  reset(); R.x[0] = gvm; armtr_regs_set(cpu, &R); fake_tramp(cpu, (uint64_t)vtbl[5]); armtr_regs_get(cpu, &R); CHECK(R.x[0] == 5);

  // ---- GetMethodID caches the descriptor; CallXMethod(...) reads regs + stack ----
  const char* d1 = "(IJFDLjava/lang/Object;Z[I)D";
  reset(); R.x[1] = 0xc1a55; R.x[2] = (uint64_t)"m"; R.x[3] = (uint64_t)d1; call("GetMethodID");
  uint64_t mid1 = R.x[0]; CHECK(mid1 == 0x5000 + strlen(d1));
  reset(); R.x[1] = 0x0b1; R.x[2] = mid1;
  R.x[3] = 0xffffffff00000011ull;      // I (upper garbage)
  R.x[4] = 0x1122334455667788ull;      // J
  setd(0, 2.5);                        // F promoted to double
  setd(1, -8.125);                     // D
  R.x[5] = 0x999; R.x[6] = 0x101;      // L, Z
  R.x[7] = 0x4000;                     // [I
  call("CallDoubleMethod");
  CHECK(getd(0) == 6.5 && last_obj == (jobject)0xb1 && last_mid == (jmethodID)mid1);
  CHECK(copy_jv[0].i == 0x11 && copy_jv[1].j == 0x1122334455667788ll && copy_jv[2].f == 2.5f && copy_jv[3].d == -8.125);
  CHECK(copy_jv[4].l == (jobject)0x999 && copy_jv[5].z == 1 && copy_jv[6].l == (jobject)0x4000);

  // more ints than registers: (IIIIIIII)I -> x3..x7 + 3 stack slots
  const char* d2 = "(IIIIIIII)I";
  uint64_t mid2 = getmid("GetMethodID", d2);
  reset(); R.x[2] = mid2; for (int i = 0; i < 5; i++) R.x[3 + i] = 100 + i; gstack[0] = 105; gstack[1] = 106; gstack[2] = 107; call("CallIntMethod");
  CHECK((int32_t)R.x[0] == -9);
  for (int i = 0; i < 8; i++) CHECK(copy_jv[i].i == 100 + i);

  // A variant: guest jvalue* passed straight through, env translated, boolean normalised
  jvalue guest_jv[2] = {}; 
  reset(); R.x[1] = 0x77; R.x[2] = mid1; R.x[3] = (uint64_t)guest_jv; call("CallBooleanMethodA");
  CHECK(R.x[0] == 0xff && last_jv == guest_jv && last_obj == (jobject)0x77);

  // V variant: va_list struct by pointer in x3
  const char* d3 = "(IDJ)V";
  uint64_t mid3 = getmid("GetStaticMethodID", d3);
  uint64_t gr[4] = {0x31, 0x33, 0, 0}; uint8_t vr[2][16] = {};
  { double d = 4.5; memcpy(vr[1], &d, 8); }
  GuestVaList l = {gstack, gr + 2, vr + 2, -16, -32};   // x6,x7 remain unread; vr: v6,v7
  gr[0] = 0x31; gr[1] = 0x33; { double d = 4.5; memcpy(vr[0], &d, 8); memset(vr[1], 0, 16); }
  // order: I <- gr[0] (0x31), D <- vr[0] (4.5), J <- gr[1] (0x33)
  reset(); R.x[1] = 0xc; R.x[2] = mid3; R.x[3] = (uint64_t)&l; call("CallStaticVoidMethodV");
  CHECK(last_cls == (jclass)0xc && last_mid == (jmethodID)mid3);
  CHECK(copy_jv[0].i == 0x31 && copy_jv[1].d == 4.5 && copy_jv[2].j == 0x33);

  // Static float return into v0 (low 32 bits)
  const char* d4 = "(F)F";
  uint64_t mid4 = getmid("GetStaticMethodID", d4);
  reset(); R.x[1] = 0xc; R.x[2] = mid4; setd(0, 0.75); call("CallStaticFloatMethod");
  { float f; memcpy(&f, R.v[0], 4); CHECK(f == 1.25f); }
  CHECK(copy_jv[0].f == 0.75f);

  // Nonvirtual: 4 fixed args, varargs start at x4
  const char* d5 = "(II)I";
  uint64_t mid5 = getmid("GetMethodID", d5);
  reset(); R.x[1] = 0x01; R.x[2] = 0x02; R.x[3] = mid5; R.x[4] = 8; R.x[5] = 9; call("CallNonvirtualIntMethod");
  CHECK(R.x[0] == 31 && last_obj == (jobject)1 && last_cls == (jclass)2 && copy_jv[0].i == 8 && copy_jv[1].i == 9);

  // NewObject(...) with a <init> descriptor
  const char* d6 = "(IF)V";
  uint64_t mid6 = getmid("GetMethodID", d6);
  reset(); R.x[1] = 0xc; R.x[2] = mid6; R.x[3] = 3; setd(0, 1.5); call("NewObject");
  CHECK(R.x[0] == 0x777 && last_cls == (jclass)0xc && copy_jv[0].i == 3 && copy_jv[1].f == 1.5f);

  // externally obtained ids
  armtr_proxy_jni_register_method((void*)0x9999, "(JJ)J");
  reset(); R.x[2] = 0x9999; call("CallIntMethod");   // host mock records 8 values; just must not crash
  puts("test_jni OK");
  return 0;
}
