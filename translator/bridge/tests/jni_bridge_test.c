// SPDX-License-Identifier: GPL-2.0-or-later
// arm64 guest JNI library for the bridge tests (host: bridge_host_test.cpp, device: the test APK).
// Java side: class ours.test.Bridge (static natives).
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <string.h>

int __android_log_print(int prio, const char* tag, const char* fmt, ...);   // liblog (guest proxy lib)

static jint add(JNIEnv* env, jclass c, jint a, jint b) { (void)env; (void)c; return a * 3 + b; }

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = (*env)->FindClass(env, "ours/test/Bridge");
  if (!c) return JNI_ERR;
  JNINativeMethod m[] = {{"add", "(II)I", (void*)add}};
  if ((*env)->RegisterNatives(env, c, m, 1) != JNI_OK) return JNI_ERR;
  return JNI_VERSION_1_6;
}

// 10 doubles + 1 float + ints/long: v0-v7 overflow to the stack on both sides, interleaved with ints.
JNIEXPORT jdouble JNICALL Java_ours_test_Bridge_mix(JNIEnv* e, jclass c, jint a, jdouble b, jfloat f, jlong l,
    jdouble d1, jdouble d2, jdouble d3, jdouble d4, jdouble d5, jdouble d6, jdouble d7, jdouble d8, jint i2,
    jint i3, jint i4, jint i5, jint i6, jint i7) {
  (void)e; (void)c;
  return a + b + f + (double)l + d1 + 2 * d2 + 3 * d3 + 4 * d4 + 5 * d5 + 6 * d6 + 7 * d7 + 8 * d8 + i2 * 10.0 +
         i3 * 100.0 + i4 * 1000.0 + i5 * 1e4 + i6 * 1e5 + i7 * 1e6;
}
JNIEXPORT jfloat JNICALL Java_ours_test_Bridge_half(JNIEnv* e, jclass c, jfloat x) { (void)e; (void)c; return x / 2; }
JNIEXPORT jboolean JNICALL Java_ours_test_Bridge_isNeg(JNIEnv* e, jclass c, jint x) { (void)e; (void)c; return x < 0; }
JNIEXPORT jshort JNICALL Java_ours_test_Bridge_neg16(JNIEnv* e, jclass c, jshort x) { (void)e; (void)c; return (jshort)-x; }

// Calls back into Java: static long cb(int, long).
JNIEXPORT jlong JNICALL Java_ours_test_Bridge_callback(JNIEnv* e, jclass c, jint x) {
  jmethodID m = (*e)->GetStaticMethodID(e, c, "cb", "(IJ)J");
  return m ? (*e)->CallStaticLongMethod(e, c, m, x, (jlong)100) + 1 : -1;
}
JNIEXPORT jstring JNICALL Java_ours_test_Bridge_hello(JNIEnv* e, jclass c) {
  (void)c;
  return (*e)->NewStringUTF(e, "hello from arm64");
}

// Guest thread (guest pthread_create -> host thread) that attaches to the VM.
static JavaVM* g_vm;
static void* thr(void* p) {
  JNIEnv* e = NULL;
  jint r = (*g_vm)->AttachCurrentThread(g_vm, &e, NULL);
  *(jint*)p = r == JNI_OK && e ? (*e)->GetVersion(e) : -1;
  (*g_vm)->DetachCurrentThread(g_vm);
  return NULL;
}
JNIEXPORT jint JNICALL Java_ours_test_Bridge_thread(JNIEnv* e, jclass c) {
  (void)c;
  if ((*e)->GetJavaVM(e, &g_vm) != JNI_OK) return -2;
  jint v = 0;
  pthread_t t;
  if (pthread_create(&t, NULL, thr, &v)) return -3;
  pthread_join(t, NULL);
  return v;
}

// Per-thread guest state: thread_local + errno must be private to each (host) thread.
static __thread int tls_counter;
JNIEXPORT jint JNICALL Java_ours_test_Bridge_tls(JNIEnv* e, jclass c) {
  (void)e; (void)c;
  errno = 1000 + tls_counter;
  int mine = ++tls_counter;
  return errno == 1000 + mine - 1 ? mine : -1;
}

// liblog through the guest-side proxy library -> host liblog (variadic, format rebuilt on the host).
JNIEXPORT jint JNICALL Java_ours_test_Bridge_log(JNIEnv* e, jclass c, jint n) {
  (void)e; (void)c;
  return __android_log_print(4, "armtr-test", "n=%d %s %.2f", n, "ok", 0.5);
}
