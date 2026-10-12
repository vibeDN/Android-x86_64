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
/* Minimal JNI .so: JNI_OnLoad registers one native method via RegisterNatives. */
#include <jni.h>

static jint nativeAdd(JNIEnv* env, jclass clazz, jint a, jint b) {
  (void)env; (void)clazz;
  return a * 3 + b;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = (*env)->FindClass(env, "ours/test/Native");
  if (!c) return JNI_ERR;
  JNINativeMethod m[] = {{"add", "(II)I", (void*)nativeAdd}};
  if ((*env)->RegisterNatives(env, c, m, 1) != JNI_OK) return JNI_ERR;
  return JNI_VERSION_1_6;
}

/* Also exported under the JNI short name, to test dlsym lookup path. */
JNIEXPORT jint JNICALL Java_ours_test_Native_add2(JNIEnv* env, jclass c, jint a, jint b) {
  return nativeAdd(env, c, a, b) + 1;
}
