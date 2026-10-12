// SPDX-License-Identifier: GPL-2.0-or-later
// Fake ART on the glibc host: dlopens libarmtr_bridge.so, drives NativeBridgeCallbacks like libnativebridge /
// libnativeloader / ART's JNI code do, against a mock host JNIEnv/JavaVM, and calls native methods of the arm64
// libjni_bridge_test.so and libjni_test.so through the returned host trampolines.
// Usage: ARMTR_SYSROOT=<guest root> bridge_host_test <libarmtr_bridge.so> <dir with the arm64 test libs>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <thread>

#include "nativebridge/native_bridge.h"

namespace android {
struct NativeBridgeRuntimeValues { const char* os_arch; const char* cpu_abi; const char* cpu_abi2;
                                   const char* const* supported_abis; int32_t abi_count; };
}
using namespace android;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)
#define PASS(name) printf("%s %s\n", g_fail ? "...." : "PASS", name)

static NativeBridgeCallbacks* nb;

// ---- mock ART JNI ----
static JNINativeInterface T;
static JNIInvokeInterface VT;
static _JavaVM HVM{&VT};
static thread_local _JNIEnv t_env{&T};   // one host JNIEnv per thread, like ART
static std::map<std::string, void*> g_natives;   // "name" -> host-callable fn (after ART's bridge handling)
static std::map<jstring, std::string> g_strings;
static std::string shorty_of(const char* sig) {   // "(IJLx;[I)V" -> "VIJLL"
  std::string a;
  const char* p = sig + 1;
  for (; *p && *p != ')'; p++) {
    char c = *p;
    while (*p == '[') { p++; c = 'L'; }
    if (*p == 'L') while (*p && *p != ';') p++;
    a += (c == 'L' || c == '[') ? 'L' : c;
  }
  char r = p[1] == '[' ? 'L' : p[1];
  return std::string(1, r) + a;
}
static jint m_GetVersion(JNIEnv* e) { return e == &t_env ? 0x10006 : -1; }
static jclass m_FindClass(JNIEnv*, const char* n) { return strncmp(n, "ours/test/", 10) ? nullptr : (jclass)0xc1a55; }
// Like art/runtime/jni/jni_internal.cc RegisterNatives: guest pointers go through the bridge.
static jint m_RegisterNatives(JNIEnv*, jclass, const JNINativeMethod* m, jint n) {
  for (int i = 0; i < n; i++) {
    void* fn = m[i].fnPtr;
    if (nb->isNativeBridgeFunctionPointer(fn)) {
      std::string s = shorty_of(m[i].signature);
      fn = nb->getTrampolineForFunctionPointer(fn, s.c_str(), (uint32_t)s.size(), kJNICallTypeRegular);
    }
    g_natives[m[i].name] = fn;
  }
  return JNI_OK;
}
static jmethodID m_GetStaticMethodID(JNIEnv*, jclass c, const char* n, const char* s) {
  return c == (jclass)0xc1a55 && !strcmp(n, "cb") && !strcmp(s, "(IJ)J") ? (jmethodID)0x99 : nullptr;
}
static jlong m_CallStaticLongMethodA(JNIEnv*, jclass, jmethodID m, const jvalue* a) {
  return m == (jmethodID)0x99 ? a[0].i * 2 + a[1].j : -1000;
}
static jstring m_NewStringUTF(JNIEnv*, const char* s) {
  jstring j = (jstring)(0x5000 + g_strings.size());
  g_strings[j] = s;
  return j;
}
static jint m_GetJavaVM(JNIEnv*, JavaVM** vm) { *vm = &HVM; return JNI_OK; }
static jint v_GetEnv(JavaVM*, void** out, jint) { *out = &t_env; return JNI_OK; }
static jint v_Attach(JavaVM*, JNIEnv** out, void*) { *out = &t_env; return JNI_OK; }
static jint v_Detach(JavaVM*) { return JNI_OK; }

template <class F> static F sym(void* h, const char* name, const char* shorty) {
  return (F)nb->getTrampolineWithJNICallType(h, name, shorty, shorty ? (uint32_t)strlen(shorty) : 0, kJNICallTypeRegular);
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s libarmtr_bridge.so test_lib_dir\n", argv[0]); return 2; }
  T.GetVersion = m_GetVersion; T.FindClass = m_FindClass; T.RegisterNatives = m_RegisterNatives;
  T.GetStaticMethodID = m_GetStaticMethodID; T.CallStaticLongMethodA = m_CallStaticLongMethodA;
  T.NewStringUTF = m_NewStringUTF; T.GetJavaVM = m_GetJavaVM;
  VT.GetEnv = v_GetEnv; VT.AttachCurrentThread = v_Attach; VT.DetachCurrentThread = v_Detach;
  std::string dir = argv[2], lib = dir + "/libjni_bridge_test.so";

  // M0: what libnativebridge / zygote / PackageManager-side code asks first.
  void* so = dlopen(argv[1], RTLD_NOW);
  if (!so) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
  nb = (NativeBridgeCallbacks*)dlsym(so, "NativeBridgeItf");
  CHECK(nb && nb->version == 8);
  CHECK(nb->isCompatibleWith(8) && nb->isCompatibleWith(2) && !nb->isCompatibleWith(9));
  const NativeBridgeRuntimeValues* env = nb->getAppEnv("arm64");
  CHECK(env && !strcmp(env->cpu_abi, "arm64-v8a") && !strcmp(env->os_arch, "aarch64"));
  CHECK(nb->getAppEnv("x86_64") == nullptr);
  CHECK(nb->isSupported(lib.c_str()));
  CHECK(!nb->isSupported(argv[1]));   // an x86_64 ELF
  CHECK(nb->isPathSupported("/data/app/~~x/pkg-1/lib/arm64") && nb->isPathSupported("/a/base.apk!/lib/arm64-v8a"));
  CHECK(!nb->isPathSupported("/system/lib64"));
  CHECK(!nb->initialize(nullptr, "/tmp", "x86_64"));
  NativeBridgeRuntimeCallbacks rcb{};
  CHECK(nb->initialize(&rcb, "/tmp/code_cache", "arm64"));
  PASS("M0 callbacks, initialize (guest linker + libc booted)");

  // M1/M2: namespaces + load, as libnativeloader does for a classloader namespace.
  native_bridge_namespace_t* def = nb->getExportedNamespace("default");
  CHECK(def != nullptr);
  CHECK(nb->getExportedNamespace("system") == nullptr);
  native_bridge_namespace_t* app = nb->createNamespace("clns-1", nullptr, dir.c_str(), 1 /*ISOLATED*/, dir.c_str(), def);
  CHECK(app != nullptr);
  CHECK(nb->linkNamespaces(app, def, "libc.so:libm.so:libdl.so:liblog.so"));
  void* h = nb->loadLibraryExt("libjni_bridge_test.so", RTLD_NOW, app);
  CHECK(h != nullptr);
  CHECK(nb->loadLibraryExt("libdoes_not_exist.so", RTLD_NOW, app) == nullptr && strstr(nb->getError(), "not found"));
  void* h2 = nb->loadLibrary((dir + "/libjni_test.so").c_str(), RTLD_NOW);   // no namespace: guest default
  CHECK(h2 != nullptr);
  PASS("M1/M2 guest namespaces + dlopen");

  // M3: JNI.
  auto onload = sym<jint (*)(JavaVM*, void*)>(h, "JNI_OnLoad", nullptr);
  CHECK(onload && onload(&HVM, nullptr) == JNI_VERSION_1_6);
  auto add = (jint(*)(JNIEnv*, jclass, jint, jint))g_natives["add"];
  CHECK(add && add(&t_env, (jclass)0xc1a55, 5, 7) == 22);
  auto mix = sym<jdouble (*)(JNIEnv*, jclass, jint, jdouble, jfloat, jlong, jdouble, jdouble, jdouble, jdouble, jdouble,
                             jdouble, jdouble, jdouble, jint, jint, jint, jint, jint, jint)>(h, "Java_ours_test_Bridge_mix",
                                                                                          "DIDFJDDDDDDDDIIIIII");
  double want = 1 + 2.5 + 0.25 + 4.0 + 1.5 + 2 * 2.5 + 3 * 3.5 + 4 * 4.5 + 5 * 5.5 + 6 * 6.5 + 7 * 7.5 + 8 * 8.5 +
                2 * 10.0 + 3 * 100.0 + 4 * 1000.0 + 5 * 1e4 + 6 * 1e5 + 7 * 1e6;
  double got = mix ? mix(&t_env, 0, 1, 2.5, 0.25f, 4, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 2, 3, 4, 5, 6, 7) : 0;
  CHECK(fabs(got - want) < 1e-6);
  auto half = sym<jfloat (*)(JNIEnv*, jclass, jfloat)>(h, "Java_ours_test_Bridge_half", "FF");
  CHECK(half && half(&t_env, 0, 3.0f) == 1.5f);
  auto isneg = sym<jboolean (*)(JNIEnv*, jclass, jint)>(h, "Java_ours_test_Bridge_isNeg", "ZI");
  CHECK(isneg && isneg(&t_env, 0, -4) == 1 && isneg(&t_env, 0, 4) == 0);
  auto neg16 = sym<jshort (*)(JNIEnv*, jclass, jshort)>(h, "Java_ours_test_Bridge_neg16", "SS");
  CHECK(neg16 && neg16(&t_env, 0, 5) == -5);
  auto cb = sym<jlong (*)(JNIEnv*, jclass, jint)>(h, "Java_ours_test_Bridge_callback", "JI");
  CHECK(cb && cb(&t_env, (jclass)0xc1a55, 5) == 111);
  auto hello = sym<jstring (*)(JNIEnv*, jclass)>(h, "Java_ours_test_Bridge_hello", "L");
  jstring s = hello ? hello(&t_env, 0) : nullptr;
  CHECK(s && g_strings[s] == "hello from arm64");
  CHECK(sym<void*>(h, "Java_no_such_method", "V") == nullptr);
  auto add2 = sym<jint (*)(JNIEnv*, jclass, jint, jint)>(h2, "Java_ours_test_Native_add2", "III");
  CHECK(add2 && add2(&t_env, 0, 5, 7) == 23);
  PASS("M3 JNI_OnLoad/RegisterNatives, by-name natives, FP/stack args, callbacks into Java, strings");

  // Threads: a guest pthread attaching to the VM; host threads entering guest code (lazy attach) with own TLS.
  auto thr = sym<jint (*)(JNIEnv*, jclass)>(h, "Java_ours_test_Bridge_thread", "I");
  CHECK(thr && thr(&t_env, 0) == 0x10006);
  auto tls = sym<jint (*)(JNIEnv*, jclass)>(h, "Java_ours_test_Bridge_tls", "I");
  CHECK(tls && tls(&t_env, 0) == 1 && tls(&t_env, 0) == 2);
  int r1 = 0, r2 = 0, r3 = 0;
  std::thread t1([&] { r1 = tls(&t_env, 0); r2 = tls(&t_env, 0); r3 = add(&t_env, 0, 1, 1); });
  t1.join();
  CHECK(r1 == 1 && r2 == 2 && r3 == 4);
  int par[4] = {0};
  std::thread ts[4];
  for (int i = 0; i < 4; i++) ts[i] = std::thread([&, i] { for (int k = 0; k < 200; k++) par[i] = tls(&t_env, 0); });
  for (auto& t : ts) t.join();
  CHECK(par[0] == 200 && par[1] == 200 && par[2] == 200 && par[3] == 200);
  CHECK(tls(&t_env, 0) == 3);
  // Guest proxy library liblog.so -> hostlibs -> (fake) host liblog.
  auto lg = sym<jint (*)(JNIEnv*, jclass, jint)>(h, "Java_ours_test_Bridge_log", "II");
  void* fl = getenv("ARMTR_HOSTLIBS_DIR") ? dlopen((std::string(getenv("ARMTR_HOSTLIBS_DIR")) + "/liblog.so").c_str(), RTLD_NOW) : nullptr;
  CHECK(lg && lg(&t_env, 0, 42) == 7);
  CHECK(fl && !strcmp((const char*)dlsym(fl, "fake_log_msg"), "n=42 ok 0.50") && !strcmp((const char*)dlsym(fl, "fake_log_tag"), "armtr-test"));
  PASS("threads: guest pthread + AttachCurrentThread, host-thread attach, per-thread guest TLS; liblog proxy lib");
  printf(g_fail ? "FAILED\n" : "ALL PASS\n");
  return g_fail;
}
