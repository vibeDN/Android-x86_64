#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Populate AOSP's external/{unicorn,armtr} and device/ours/pc_x86_64/translator from this repo.
# Prereq: scripts/fetch-unicorn.sh (patched checkout in translator/third_party-build/unicorn/src).
# Usage: scripts/sync-translator-to-aosp.sh [AOSP_DIR]   
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
A="${1:-/mnt/storage-adata/aosp15}"
U="$R/translator/third_party-build/unicorn/src"
[ -f "$U/uc.c" ] || { echo "run scripts/fetch-unicorn.sh first" >&2; exit 1; }
grep -q "return CF_PARALLEL" "$U/qemu/include/exec/exec-all.h" || { echo "Unicorn not CF_PARALLEL-patched" >&2; exit 1; }
D=device/ours/pc_x86_64/translator
# Soong does not descend into symlinked directories (tested: Android.bp behind a dir symlink is
# ignored), so everything is a real rsync copy. Re-run after editing translator/ or the device dir.
mkdir -p "$A/external/unicorn" "$A/external/armtr" "$A/$D"
for l in "$A/external/armtr" "$A/$D"; do [ -L "$l" ] && rm "$l" && mkdir -p "$l"; done
rsync -a --delete --exclude .git --exclude Android.bp "$U/" "$A/external/unicorn/"
cp -f "$R/$D/unicorn.Android.bp" "$A/external/unicorn/Android.bp"
rsync -a --delete --exclude third_party-build --exclude tests "$R/translator/" "$A/external/armtr/"
rsync -a --delete "$R/$D/" "$A/$D/"
# art's libnativebridge-headers is not visible outside art/ and include_dirs on art/ is neverallow:
# take a version-matched copy of the (Apache-2.0) header into the AOSP-side armtr copy only.
mkdir -p "$A/external/armtr/bridge/include/nativebridge"
cp -f "$A/art/libnativebridge/include/nativebridge/native_bridge.h" "$A/external/armtr/bridge/include/nativebridge/"
echo "synced into $A"
