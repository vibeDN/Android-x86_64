/*
 * Copyright (C) 2026 The Android-x86_64 Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* Fake-VM host for jni_test.so: dlopen, call JNI_OnLoad with a stub JavaVM/JNIEnv,
 * capture RegisterNatives, invoke the native method. No ART needed. */
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

static JNINativeMethod g_m; static int g_n;
static jclass FindClass(JNIEnv* e, const char* n) { (void)e; return (jclass)(n ? (void*)n : 0); }
static jint RegisterNatives(JNIEnv* e, jclass c, const JNINativeMethod* m, jint n) {
  (void)e; (void)c; g_m = m[0]; g_n = n; return JNI_OK;
}
static struct JNINativeInterface fn;
static const struct JNINativeInterface* envp = &fn;
static jint GetEnv(JavaVM* vm, void** out, jint v) { (void)vm; (void)v; *out = &envp; return JNI_OK; }
static struct JNIInvokeInterface ifn;
static const struct JNIInvokeInterface* vmp = &ifn;

int main(int argc, char** argv) {
  void* h = dlopen(argc > 1 ? argv[1] : "./libjni_test.so", RTLD_NOW);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
  fn.FindClass = FindClass; fn.RegisterNatives = RegisterNatives; ifn.GetEnv = GetEnv;
  jint (*onload)(JavaVM*, void*) = (jint(*)(JavaVM*, void*))dlsym(h, "JNI_OnLoad");
  jint (*sn)(JNIEnv*, jclass, jint, jint) = (jint(*)(JNIEnv*, jclass, jint, jint))dlsym(h, "Java_ours_test_Native_add2");
  if (!onload || !sn) { printf("dlsym failed\n"); return 3; }
  jint v = onload((JavaVM*)&vmp, 0);
  jint (*f)(JNIEnv*, jclass, jint, jint) = (jint(*)(JNIEnv*, jclass, jint, jint))g_m.fnPtr;
  jint r = f((JNIEnv*)&envp, 0, 5, 7), r2 = sn((JNIEnv*)&envp, 0, 5, 7);
  printf("onload=0x%x n=%d name=%s sig=%s add(5,7)=%d add2=%d\n", v, g_n, g_m.name, g_m.signature, r, r2);
  return (v == JNI_VERSION_1_6 && r == 22 && r2 == 23) ? 0 : 1;
}
