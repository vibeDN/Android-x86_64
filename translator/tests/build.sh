#!/bin/bash
# Build ARM64 Bionic test programs with AOSP clang, no Soong. Usage: build.sh [OUT]
# Links against prebuilt stub libc/libm/libdl from the runtime mainline sdk snapshot.
set -e
T=${AOSP:-/mnt/storage-adata/aosp15}
OUT=${1:-/mnt/storage-adata/translator-research/tests-out}
HERE=$(cd "$(dirname "$0")" && pwd)
CLANG=$(echo $T/prebuilts/clang/host/linux-x86/clang-r563880)
SDK=$T/prebuilts/runtime/mainline/runtime/sdk/android/arm64/lib
B=$T/bionic/libc
BI=$(ls -d $CLANG/lib/clang/*/lib/linux)/libclang_rt.builtins-aarch64-android.a
mkdir -p "$OUT"
CF="--target=aarch64-linux-android35 -nostdlibinc -O2 -fPIC -march=armv8-a+crypto+crc
 -isystem $B/include -isystem $B/kernel/uapi -isystem $B/kernel/uapi/asm-arm64 -isystem $B/kernel/android/uapi
 -isystem $T/libnativehelper/include_jni"
LF="-fuse-ld=lld -nostdlib -Wl,-z,max-page-size=16384 -Wl,--build-id=none"
EXE="$SDK/crtbegin_dynamic.o"; EXE_END="$SDK/crtend_android.o"
LIBS="$SDK/libm.so $SDK/libdl.so $SDK/libc.so $BI"
for p in hello stress pthread_test; do
  $CLANG/bin/clang $CF -c $HERE/$p.c -o $OUT/$p.o
  $CLANG/bin/clang $CF $LF -pie -Wl,--dynamic-linker=/system/bin/linker64 \
    $EXE $OUT/$p.o $LIBS $EXE_END -o $OUT/${p}_dyn
done
$CLANG/bin/clang $CF -c $HERE/jni_test.c -o $OUT/jni_test.o
$CLANG/bin/clang $CF $LF -shared -Wl,-soname,libjni_test.so $SDK/crtbegin_so.o $OUT/jni_test.o $LIBS $SDK/crtend_so.o -o $OUT/libjni_test.so
$CLANG/bin/clang $CF -c $HERE/jni_host.c -o $OUT/jni_host.o
$CLANG/bin/clang $CF $LF -pie -Wl,--dynamic-linker=/system/bin/linker64 $EXE $OUT/jni_host.o $LIBS $EXE_END -o $OUT/jni_host_dyn
echo built in $OUT
