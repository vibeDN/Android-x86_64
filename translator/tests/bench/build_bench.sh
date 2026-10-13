#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the benchmark set (docs/translator/06-performance.md) for arm64 bionic (AOSP clang, prebuilt stub libs,
# flags as translator/tests/build.sh) and natively for x86_64 glibc. sqlite/zlib are compiled from AOSP
# external/ sources (read only). Usage: build_bench.sh [OUT]  -> OUT/{arm64,x86_64}/{bench,dbz_bench}
set -e
A=${AOSP:-/mnt/storage-adata/aosp15}
OUT=${1:-/mnt/storage-adata/tmp/bench-out}
HERE=$(cd "$(dirname "$0")" && pwd)
CL=$A/prebuilts/clang/host/linux-x86/clang-r563880/bin/clang
SDK=$A/prebuilts/runtime/mainline/runtime/sdk/android/arm64/lib; B=$A/bionic/libc
BI=$(ls -d $(dirname $CL)/../lib/clang/*/lib/linux)/libclang_rt.builtins-aarch64-android.a
SQ=$A/external/sqlite/dist/sqlite-default; Z=$A/external/zlib
ZSRC="adler32.c compress.c cpu_features.c crc32.c deflate.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
SQDEF="-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0"
mkdir -p $OUT/arm64 $OUT/x86_64
ACF="--target=aarch64-linux-android35 -nostdlibinc -O2 -fPIC -march=armv8-a+crc -isystem $B/include
 -isystem $B/kernel/uapi -isystem $B/kernel/uapi/asm-arm64 -isystem $B/kernel/android/uapi"
ALF="-fuse-ld=lld -nostdlib -pie -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,max-page-size=16384"
ALIBS="$SDK/libm.so $SDK/libdl.so $SDK/libc.so $BI"
build() {  # arch cc cflags ldpre ldpost
  local o=$OUT/$1; shift; local cc=$1 cf=$2 pre=$3 post=$4
  $cc $cf -c $HERE/bench.c -o $o/bench.o
  $cc $cf $pre $o/bench.o $post -o $o/bench
  [ -f $o/sqlite3.o ] || $cc $cf $SQDEF -c $SQ/sqlite3.c -o $o/sqlite3.o
  for f in $ZSRC; do [ -f $o/z_$f.o ] || $cc $cf -I$Z -c $Z/$f -o $o/z_$f.o; done
  $cc $cf -I$SQ -I$Z -c $HERE/dbz_bench.c -o $o/dbz_bench.o
  $cc $cf $pre $o/dbz_bench.o $o/sqlite3.o $o/z_*.o $post -o $o/dbz_bench
}
build arm64 $CL "$ACF" "$ALF $SDK/crtbegin_dynamic.o" "$ALIBS $SDK/crtend_android.o"
build x86_64 $CL "-O2 -march=x86-64-v2" "-fuse-ld=lld" "-lpthread -lm"
echo "built in $OUT"
