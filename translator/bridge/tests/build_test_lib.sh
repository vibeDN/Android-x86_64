#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the arm64 test JNI library against our guest Bionic. Usage: build_test_lib.sh OUT_DIR
set -e
T=${AOSP:-/mnt/storage-adata/aosp15}
S=${GUEST_SYSROOT:-/mnt/storage-adata/translator-research/sysroot-ours}
OUT=${1:?out dir}; HERE=$(cd "$(dirname "$0")" && pwd)
CL=$T/prebuilts/clang/host/linux-x86/clang-r563880/bin/clang
B=$T/bionic/libc
BI=$(ls -d $T/prebuilts/clang/host/linux-x86/clang-r563880/lib/clang/*/lib/linux)/libclang_rt.builtins-aarch64-android.a
mkdir -p "$OUT"
$CL --target=aarch64-linux-android35 -nostdlibinc -O2 -fPIC -isystem $B/include -isystem $B/kernel/uapi \
  -isystem $B/kernel/uapi/asm-arm64 -isystem $B/kernel/android/uapi -isystem $T/libnativehelper/include_jni \
  -fuse-ld=lld -nostdlib -shared -Wl,-z,max-page-size=16384 -Wl,-soname,libjni_bridge_test.so \
  $S/crt/crtbegin_so.o "$HERE/jni_bridge_test.c" $S/system/lib64/libc.so $S/system/lib64/libdl.so ${GUEST_LIBLOG:-} $BI \
  $S/crt/crtend_so.o -o "$OUT/libjni_bridge_test.so"
