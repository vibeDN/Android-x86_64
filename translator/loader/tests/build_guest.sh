#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# usage: build_guest.sh <outdir>   builds aarch64 test objects in several linker variants.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)/guest"; OUT="$1"
CLANG=$(ls -d /mnt/storage-adata/aosp15/prebuilts/clang/host/linux-x86/clang-r*/bin/clang 2>/dev/null | tail -1)
[ -x "$CLANG" ] || { echo "no AOSP clang"; exit 77; }
mkdir -p "$OUT"
build() {  # name extra-flags
  d="$OUT/$1"; shift; mkdir -p "$d"
  F="--target=${TGT:-aarch64-linux-gnu} -fPIC -O1 -nostdlib -fuse-ld=lld -fno-stack-protector -Wl,--allow-shlib-undefined -Wl,-init=a_init $*"
  $CLANG $F -shared -Wl,-soname,liba.so "$SRC/a.c" -o "$d/liba.so"
  $CLANG $F -shared -Wl,-soname,libb.so "$SRC/b.c" -L"$d" -l:liba.so -o "$d/libb.so"
  $CLANG $F -pie -Wl,--export-dynamic "$SRC/exe.c" -L"$d" -l:liba.so -o "$d/exe"
}
build base -Wl,--hash-style=both
build gnu -Wl,--hash-style=gnu
build sysv -Wl,--hash-style=sysv
build relr -Wl,--pack-dyn-relocs=relr -Wl,--hash-style=gnu
build big64k -Wl,-z,max-page-size=65536
build symb -Wl,-Bsymbolic
build small4k -Wl,-z,max-page-size=4096 -Wl,-z,separate-code
TGT=aarch64-linux-android29 build android -Wl,--pack-dyn-relocs=android+relr
# static-pie with a local ifunc: yields R_AARCH64_IRELATIVE
mkdir -p "$OUT/ifunc"
$CLANG --target=aarch64-linux-gnu -fPIC -O0 -nostdlib -static-pie -fuse-ld=lld "$SRC/ifunc.c" -o "$OUT/ifunc/exe"
