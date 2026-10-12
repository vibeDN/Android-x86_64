#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Assembles the guest (arm64) file set of the NativeBridge in device layout:
#   OUT/system/bin/armtr/{linker64,armtr_guest}  OUT/system/lib64/armtr/*.so  OUT/system/etc/ld.config.arm64.txt
# Inputs: our arm64 Bionic (docs/translator/04-arm64-bionic-build.md), AOSP clang. No Soong.
# Host tests use OUT as --sysroot; scripts/sync-translator-to-aosp.sh copies it into the device dir (prebuilt_any).
# Usage: build_guest.sh [OUT]
set -e
T=${AOSP:-/mnt/storage-adata/aosp15}
S=${GUEST_SYSROOT:-/mnt/storage-adata/translator-research/sysroot-ours}
OUT=${1:-/mnt/storage-adata/tmp/armtr-guest}
HERE=$(cd "$(dirname "$0")" && pwd)
CL=$T/prebuilts/clang/host/linux-x86/clang-r563880/bin/clang
B=$T/bionic/libc
mkdir -p "$OUT/system/bin/armtr" "$OUT/system/lib64/armtr" "$OUT/system/etc"
CF="--target=aarch64-linux-android35 -nostdlibinc -O2 -fPIC -isystem $B/include -isystem $B/kernel/uapi
 -isystem $B/kernel/uapi/asm-arm64 -isystem $B/kernel/android/uapi"
$CL $CF -fuse-ld=lld -nostdlib -pie -Wl,-z,max-page-size=16384 -Wl,--build-id=none \
  -Wl,--dynamic-linker=/system/bin/armtr/linker64 \
  $S/crt/crtbegin_dynamic.o "$HERE/armtr_guest.c" $S/system/lib64/libc.so $S/crt/crtend_android.o \
  -o "$OUT/system/bin/armtr/armtr_guest"
cp -f $S/system/bin/linker64 "$OUT/system/bin/armtr/linker64"
for l in libc libm libdl ld-android libc++; do cp -f $S/system/lib64/$l.so "$OUT/system/lib64/armtr/"; done
[ -f $S/system/lib64/libdl_android.so ] && cp -f $S/system/lib64/libdl_android.so "$OUT/system/lib64/armtr/"
cp -f "$HERE/ld.config.arm64.txt" "$OUT/system/etc/ld.config.arm64.txt"
# Guest-side proxy stubs of host platform libraries (liblog, libandroid, ...): see gen_stubs.py
if [ -x "$HERE/gen_stubs.py" ]; then python3 "$HERE/gen_stubs.py" "$OUT/system/lib64/armtr"; fi
echo "guest root: $OUT"
