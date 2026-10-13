#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Clone Unicorn 2.1.4 and build it (aarch64 only, -j2) into translator/third_party-build/unicorn.
# Not vendored. Then: cmake -S translator/engine -B out -DUNICORN_DIR=$PWD/translator/third_party-build/unicorn
set -e
TAG=2.1.4
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$(cd "$(dirname "$0")/.." && pwd)/translator/third_party-build"
mkdir -p "$DEST"
cd "$DEST"
[ -d unicorn/src ] || { mkdir -p unicorn && git clone --depth 1 --branch "$TAG" https://github.com/unicorn-engine/unicorn.git unicorn/src; }
# Unicorn hard-codes curr_cflags() = 0, i.e. no CF_PARALLEL: ldxr/stxr and LSE atomics are NOT atomic
# between engines (guest threads). Generate parallel-safe code.
python3 - unicorn/src/qemu/include/exec/exec-all.h <<'P'
import sys,re
p=sys.argv[1]; s=open(p).read()
a="static inline uint32_t curr_cflags(void)\n{\n    return 0;\n}"
if a in s: open(p,'w').write(s.replace(a,a.replace("return 0;","return CF_PARALLEL;")))
P
# Unicorn's atomic xchg helper (SWP/SWPAL, used by bionic's pthread mutex) is a plain load+store: a lost
# update between guest threads deadlocks pthread_mutex. Upstream QEMU uses an atomic exchange.
python3 - unicorn/src/qemu/accel/tcg/atomic_template.h <<'P'
import sys
p=sys.argv[1]; s=open(p).read()
a="    ret = *haddr;\n    *haddr = val;\n"
b="    ret = atomic_xchg__nocheck(haddr, val);\n"
if a in s: open(p,'w').write(s.replace(a,b))
P
# Performance (docs/translator/06-performance.md): direct guest memory access without the softmmu TLB
# (uc->armtr_direct_mem, set by translator/engine), TBI in the translator, IC IVAU -> engine callback, cross-page
# TB chaining, inline TB-start exit check, notdirty slow path only for pages holding code, no pc restore per TLB
# fill, cached exit TB between uc_emu_start calls, direct DC ZVA/atomics, SSE4.2 crc32c.
P="$HERE/unicorn-armtr-perf.patch"
git -C unicorn/src apply --reverse --check "$P" 2>/dev/null || git -C unicorn/src apply "$P"
mkdir -p unicorn/build && cd unicorn/build
cmake ../src -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF
cmake --build . -j2
echo "Unicorn built: $DEST/unicorn"
