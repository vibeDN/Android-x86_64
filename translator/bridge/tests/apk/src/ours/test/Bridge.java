// SPDX-License-Identifier: GPL-2.0-or-later
package ours.test;

/** Natives implemented by the arm64 libjni_bridge_test.so (translator/bridge/tests/jni_bridge_test.c). */
public class Bridge {
  static native int add(int a, int b);   // registered by JNI_OnLoad (RegisterNatives)
  static native double mix(int a, double b, float f, long l, double d1, double d2, double d3, double d4,
      double d5, double d6, double d7, double d8, int i2, int i3, int i4, int i5, int i6, int i7);
  static native float half(float x);
  static native boolean isNeg(int x);
  static native short neg16(short x);
  static native long callback(int x);
  static native String hello();
  static native int thread();
  static native int tls();
  static native int log(int n);

  static long cb(int x, long y) { return x * 2 + y; }
}
