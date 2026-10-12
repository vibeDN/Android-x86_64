// SPDX-License-Identifier: GPL-2.0-or-later
// Probe an arbitrary arm64 JNI library through the bridge on the glibc host: load it into a classloader-like
// namespace and run JNI_OnLoad against a mock JNIEnv that logs every JNI call (FindClass/RegisterNatives names...).
// Usage: ARMTR_SYSROOT=<guest root> probe_lib <libarmtr_bridge.so> <lib dir> <libname.so> [<libname.so>...]
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include <utility>

#include "nativebridge/native_bridge.h"
using namespace android;

static const char* const kNames[] = {
#define JNI_RESERVED(n) #n,
#define JNI_FN(n, r, a) #n,
#include "jni_table.inc"
};
constexpr int kN = sizeof(kNames) / sizeof(kNames[0]);
static char g_fake[4096];
static NativeBridgeCallbacks* nb;
#include <map>
#include <string>
#include <stdlib.h>
#include <unistd.h>
static std::map<std::string, std::pair<void*, std::string>> g_nat;   // registered natives: name -> (fn, signature)
static const char* g_utf = "";
static std::string shorty_of(const char* sig) {
  std::string a; const char* p = sig + 1;
  for (; *p && *p != ')'; p++) { char c = *p; while (*p == '[') { p++; c = 'L'; } if (*p == 'L') while (*p && *p != ';') p++; a += c == 'L' ? 'L' : c; }
  return std::string(1, p[1] == '[' ? 'L' : p[1]) + a;
}

template <int I> static void* slot(JNIEnv*, void* a1, void* a2, void* a3, void* a4) {
  const char* n = kNames[I];
  if (!strcmp(n, "GetVersion")) return (void*)0x10006;
  if (!strncmp(n, "Exception", 9)) return nullptr;
  if (!strcmp(n, "RegisterNatives")) {
    auto* m = (const JNINativeMethod*)a2;
    printf("  JNI RegisterNatives(%p, n=%ld):", a1, (long)a3);
    for (long i = 0; i < (long)a3; i++) {
      bool g = nb->isNativeBridgeFunctionPointer(m[i].fnPtr);
      printf(" %s%s", m[i].name, g ? "" : "(!host ptr)");
      g_nat[m[i].name] = {m[i].fnPtr, m[i].signature};
    }
    printf("\n");
    return nullptr;
  }
  if (!strcmp(n, "GetStringUTFChars")) return (void*)g_utf;
  if (!strcmp(n, "ReleaseStringUTFChars") || !strcmp(n, "DeleteLocalRef")) return nullptr;
  if (!strcmp(n, "NewStringUTF")) { printf("  -> NewStringUTF(\"%.300s\")\n", (const char*)a1); fflush(stdout); return g_fake; }
  bool str = !strcmp(n, "FindClass") || strstr(n, "MethodID") || strstr(n, "FieldID");
  printf("  JNI %s(%p, %s)\n", n, a1, str ? (const char*)(strstr(n, "ID") ? a3 : a1) : "");
  if (!strcmp(n, "GetJavaVM")) { *(void**)a1 = nullptr; return (void*)-1; }   // JNI_ERR: not provided
  return g_fake;
}
template <size_t... I> static void fill(void** t, std::index_sequence<I...>) { ((t[I] = (void*)&slot<(int)I>), ...); }

static JNINativeInterface T;
static _JNIEnv ENV{&T};
static jint v_GetEnv(JavaVM*, void** out, jint) { *out = &ENV; return JNI_OK; }
static jint v_Attach(JavaVM*, JNIEnv** out, void*) { *out = &ENV; return JNI_OK; }
static JNIInvokeInterface VT;
static _JavaVM VM{&VT};

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s bridge.so libdir lib.so...\n", argv[0]); return 2; }
  fill((void**)&T, std::make_index_sequence<kN>());
  VT.GetEnv = v_GetEnv; VT.AttachCurrentThread = v_Attach;
  void* so = dlopen(argv[1], RTLD_NOW);
  nb = so ? (NativeBridgeCallbacks*)dlsym(so, "NativeBridgeItf") : nullptr;
  NativeBridgeRuntimeCallbacks rcb{};
  if (!nb || !nb->initialize(&rcb, "/tmp", "arm64")) { printf("bridge init failed\n"); return 1; }
  auto* def = nb->getExportedNamespace("default");
  auto* ns = nb->createNamespace("clns", nullptr, argv[2], 1, argv[2], def);
  // system_exposed_libraries of public.libraries.txt (subset the guest root provides)
  nb->linkNamespaces(ns, def, "libandroid.so:libc.so:libdl.so:libEGL.so:libGLESv1_CM.so:libGLESv2.so:libGLESv3.so:"
                              "libjnigraphics.so:liblog.so:libm.so:libnativewindow.so:libOpenSLES.so:libz.so");
  int rc = 0;
  for (int i = 3; i < argc; i++) {
    printf("== %s\n", argv[i]);
    fflush(stdout);
    void* h = nb->loadLibraryExt(argv[i], RTLD_NOW, ns);
    if (!h) { printf("  load FAILED: %s\n", nb->getError()); rc = 1; continue; }
    printf("  loaded\n");
    auto onload = (jint(*)(JavaVM*, void*))nb->getTrampolineWithJNICallType(h, "JNI_OnLoad", nullptr, 0,
                                                                              kJNICallTypeRegular);
    if (!onload) { printf("  no JNI_OnLoad\n"); continue; }
    fflush(stdout);
    printf("  JNI_OnLoad -> 0x%x\n", onload(&VM, nullptr));
  }
  // PROBE_TD=1: drive TDLib's JsonClient natives (static String execute(String), int createClientId(),
  // void send(int, String), String receive(double)).
  if (getenv("PROBE_TD") && g_nat.count("execute")) {
    auto tramp = [](const char* name) {
      auto& e = g_nat[name]; std::string s = shorty_of(e.second.c_str());
      return nb->getTrampolineForFunctionPointer(e.first, s.c_str(), (uint32_t)s.size(), kJNICallTypeRegular);
    };
    auto execute = (jstring(*)(JNIEnv*, jclass, jstring))tramp("execute");
    auto create = (jint(*)(JNIEnv*, jclass))tramp("createClientId");
    auto send = (void(*)(JNIEnv*, jclass, jint, jstring))tramp("send");
    auto receive = (jstring(*)(JNIEnv*, jclass, jdouble))tramp("receive");
    g_utf = "{\"@type\":\"getOption\",\"name\":\"version\"}";
    printf("execute(getOption version):\n"); fflush(stdout);
    execute(&ENV, nullptr, (jstring)g_fake);
    g_utf = "{\"@type\":\"setLogVerbosityLevel\",\"new_verbosity_level\":1}";
    execute(&ENV, nullptr, (jstring)g_fake);
    jint id = create(&ENV, nullptr);
    printf("createClientId -> %d\n", id); fflush(stdout);
    g_utf = "{\"@type\":\"getOption\",\"name\":\"version\",\"@extra\":1}";
    send(&ENV, nullptr, id, (jstring)g_fake);
    for (int i = 0; i < 6; i++) { printf("receive(1.0):\n"); fflush(stdout); receive(&ENV, nullptr, 1.0); }
  }
  return rc;
}
