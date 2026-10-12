#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Populate AOSP's external/{unicorn,armtr} and device/ours/pc_x86_64/translator from this repo.
# Prereq: scripts/fetch-unicorn.sh (patched checkout in translator/third_party-build/unicorn/src).
# Usage: scripts/sync-translator-to-aosp.sh [AOSP_DIR]   (symlinks by default; COPY=1 uses rsync)
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
A="${1:-/mnt/storage-adata/aosp15}"
U="$R/translator/third_party-build/unicorn/src"
[ -f "$U/uc.c" ] || { echo "run scripts/fetch-unicorn.sh first" >&2; exit 1; }
grep -q "return CF_PARALLEL" "$U/qemu/include/exec/exec-all.h" || { echo "Unicorn not CF_PARALLEL-patched" >&2; exit 1; }
D=device/ours/pc_x86_64/translator
# Unicorn: real copy (no .git), with our Android.bp symlinked in. Needs a copy because Soong srcs must live
# beside the Android.bp.
mkdir -p "$A/external/unicorn"
rsync -a --delete --exclude .git --exclude Android.bp "$U/" "$A/external/unicorn/"
ln -sfn "$R/$D/unicorn.Android.bp" "$A/external/unicorn/Android.bp"
# armtr + device dir: symlink, or rsync with COPY=1 (when Soong does not follow the symlinks)
if [ -n "$COPY" ]; then
  [ -L "$A/external/armtr" ] && rm "$A/external/armtr"
  rsync -a --delete --exclude third_party-build --exclude tests "$R/translator/" "$A/external/armtr/"
  mkdir -p "$A/$D"; rsync -a --delete "$R/$D/" "$A/$D/"
else
  ln -sfn "$R/translator" "$A/external/armtr"
  ln -sfn "$R/$D" "$A/$D"
fi
echo "synced into $A"
