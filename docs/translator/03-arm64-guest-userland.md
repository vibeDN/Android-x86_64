# 03 - ARM64 guest userland (Bionic, linker, test binaries)

Status: researched 2026-10-12. Everything marked VERIFIED was run on the host; ESTIMATE was not.

## Recommendation

1. **Now (milestones M1..M5): use the prebuilt arm64 `com.android.runtime` apex + runtime mainline sdk snapshot** in
   `aosp15/prebuilts/runtime/mainline/runtime/`. It has real arm64 `linker64`, `libc.so`, `libm.so`, `libdl.so`,
   `libc++.so`, `ld-android.so`, crt objects and stub libs. No build needed, zero RAM. VERIFIED: dynamic Bionic hello,
   NEON/crypto stress, pthread test and a JNI .so all run under `qemu-aarch64` with the real linker64.
2. **Later (ABI-exact for apps): one Soong build of `module_arm64only` into a separate OUT_DIR** (plan below, not run
   because it is heavy). Needed because the prebuilt is older than our tree (see "Caveat") and because
   **static Bionic (`libc.a`, `crtbegin_static.o`) exists nowhere in prebuilts**.
3. libc++_shared.so (arm64, NDK flavor) is already in `prebuilts/clang/host/linux-x86/clang-r563880/android_libc++/ndk/aarch64/lib/`.
   NDK in `prebuilts/ndk/current` is useless (only `cxx-stl/system` and `cpufeatures` sources; no sysroot/libc.a).
4. liblog: not in the prebuilt apex (it is a platform lib). Either build `liblog` in step 2, or write a tiny stub .so
   (`__android_log_print`, `__android_log_write`, `__android_log_vprint`, `__android_log_buf_print`, `__android_log_assert`) proxied to the host.

## What is really in prebuilts (VERIFIED)

| Path | Content |
|---|---|
| `prebuilts/runtime/mainline/runtime/apex/com.android.runtime-arm64.apex` (13.9 MB, build 13507691) | ext2 payload: `bin/linker64`, `bin/linker`, `bin/linkerconfig`, `lib64/bionic/{libc,libm,libdl,libdl_android}.so` (real, 1 MB libc), `lib64/{ld-android,libc++,libbase,libunwindstack,libc_malloc_debug,libc_malloc_hooks}.so`, `lib/` = arm32 |
| `.../runtime/sdk/android/arm64/lib/` | stub `libc.so libm.so libdl.so libdl_android.so` (link-time only, 246 KB libc), `crtbegin_{dynamic,so}.o`, `crtend_{android,so}.o`, `libasync_safe.a`. Headers: `include/bionic` only |
| `prebuilts/vndk/v34/arm64` | Android 14 VNDK libs, **no libc/linker**. Not useful |
| `prebuilts/ndk/current` | only cxx-stl/system + cpufeatures sources. No libc.a, no sysroot |
| `clang-r563880/android_libc++/ndk/aarch64/lib` | `libc++_shared.so`, `libc++_static.a`, `libc++abi.a` |
| `clang-r563880/android_libc++/platform/aarch64/lib` | `libc++.so` (platform flavor, differs from apex copy) |
| `clang-r563880/lib/clang/*/lib/linux/libclang_rt.builtins-aarch64-android.a` | builtins for linking |

Extract the apex without root (no debugfs on host, `7z` works):
```
mkdir -p $SCR/apex && cd $SCR/apex
unzip -o -q /mnt/storage-adata/aosp15/prebuilts/runtime/mainline/runtime/apex/com.android.runtime-arm64.apex apex_payload.img
7z x -y -ox apex_payload.img
```
(Alternative: `deapexer` from `system/apex/tools` in the tree.)

### Caveat: prebuilt is NOT ABI-identical to our tree
The prebuilt libc.so exports versions up to `LIBC_U` only (API 34). Our `bionic/libc/libc.map.txt` also has `LIBC_V`
(introduced=35: `epoll_pwait2`, `_Fork`, `android_crash_detail_*`, `mktime_z`, ...). VERIFIED with `llvm-nm -D`: 0 of the first
8 LIBC_V symbols present. Apps/NDK libs built for API 35+ that touch those will fail to resolve. Translator logic and
core libc paths are unaffected, so it is fine for CPU-engine milestones but not for the final guest userland.

## Option A: Soong build, separate OUT_DIR (PLAN, not run)

Facts (VERIFIED): `build/make/target/product/module_arm64only.mk` exists (the product used for mainline module
snapshots; 64-bit only, includes module_common.mk, 16K max page size). Release configs: `trunk_staging` (what our x86_64 product uses), `bp4a`, etc.
Existing x86_64 build for reference: ninja phase peaked at 13.4 GB maxrss at -j4 in `out/soong.log`; soong generated ~2 GB of .ninja files.

ESTIMATE (analysis is always whole-tree in Soong; `UNBUNDLED` skips kati/product packaging only, not Soong analysis):
- Soong analysis (soong_build): 4-8 GB RAM, 4-10 min, single-thread-ish. Same cost as the x86_64 product.
- Compile of libc+libm+libdl+linker+libc++ (+ deps libbase, liblog, libunwindstack, libasync_safe, ...): ~2000-3000 actions, -j2: 20-40 min, < 3 GB RAM. ccache not used by Soong by default.
- Disk: ~5-10 GB (out dir of module-only build). Peak RAM is the analysis; keep nothing else heavy running.

Commands (only after the guard passes):
```
ps -eo comm | grep -E 'ninja|soong_build' && echo BUSY-STOP || true     # must print nothing
cd /mnt/storage-adata/aosp15
export OUT_DIR=/mnt/storage-adata/aosp-out-arm64          # separate dir, does not touch out/
TARGET_PRODUCT=module_arm64only TARGET_RELEASE=trunk_staging TARGET_BUILD_VARIANT=userdebug \
TARGET_BUILD_UNBUNDLED=true \
build/soong/soong_ui.bash --make-mode -j2 libc libm libdl linker64 libc++ liblog crtbegin_dynamic crtbegin_static
```
Static libc.a (not an install target) comes from the intermediates:
`$OUT_DIR/soong/.intermediates/bionic/libc/libc/android_arm64_armv8-a_static/libc.a` (add it as an explicit goal path).
Linker and libs install under `$OUT_DIR/target/product/module_arm64only/system/{bin,lib64}` (linker64 is in the runtime apex
payload in the full product; with `m linker64` it lands in system/bin). Paths are unverified until the build is run.
Alternative on `-j2` OOM risk: `mm`-style (`m` from bionic dir) does not shrink analysis; there is no smaller path.
Cuttlefish `aosp_cf_arm64_phone` or `aosp_arm64` would work but add kati+product packaging for no gain.

## Option B: prebuilt only (what the tests below use) - VERIFIED
Sysroot for qemu (guest paths as the linker expects them):
```
S=/mnt/storage-adata/translator-research/sysroot; R=<extracted apex dir>
mkdir -p $S/system/bin $S/system/lib64 $S/apex/com.android.runtime $S/data/local/tmp $S/linkerconfig
cp $R/bin/linker64 $S/system/bin/; cp -r $R/bin $R/lib64 $S/apex/com.android.runtime/
cp $R/lib64/bionic/*.so $R/lib64/ld-android.so $R/lib64/libc++.so $S/system/lib64/
cp .../clang-r563880/android_libc++/ndk/aarch64/lib/libc++_shared.so $S/system/lib64/
qemu-aarch64 -cpu max -L $S ./hello_dyn
```
Only warning: missing `/linkerconfig/ld.config.txt` (linker falls back to its built-in default config; fine). Use real copies, not symlinks (qemu -L does not rewrite absolute symlink targets).

## Test programs
Sources: `/mnt/storage-adata/Android-x86_64/translator/tests/` (Apache-2.0). Build with
`translator/tests/build.sh [OUT]` (AOSP clang r563880, no Soong; headers from `bionic/libc/include` + kernel uapi + `libnativehelper/include_jni`,
link against the sdk stub libs and crt objects, `-Wl,--dynamic-linker=/system/bin/linker64`). Outputs: `/mnt/storage-adata/translator-research/tests-out/`.

| Test | Binary | Status |
|---|---|---|
| static raw-syscall hello | (existing) | works |
| dynamic Bionic hello | `hello_dyn` | PASS under qemu (needs linker64 + libc/libm/libdl) |
| static Bionic hello | `hello.c` | needs `libc.a`+`crtbegin_static.o` from Option A, then `clang ... -static` |
| NEON/AES/SHA/PMULL/CRC32/float stress | `stress_dyn` | PASS; golden output below |
| pthread (mutex, condvar, TLS, key, atomics) | `pthread_test_dyn` | PASS, rc 0 |
| JNI .so + fake-VM host | `libjni_test.so` + `jni_host_dyn` | PASS: `onload=0x10006 n=1 name=add sig=(II)I add(5,7)=22 add2=23` |

Golden `stress_dyn` output (qemu-aarch64 -cpu max, a correct translator must match bit for bit):
```
aes    95af22c681e1c8a5
sha    4fdb2807a6f75f96
pmull  ea7cc9df68107008 00962335c6bedea6
crc32  02ff48c1
neon   332281960 332281960 250.749985 332834.468750
double 2993.412231619840
fcvt   -3 1000000000000000000 3900000000 16777216.0
```
Note: the "sha" line is not real SHA, just a deterministic exercise of the instructions; the golden values are qemu's, not hardware's.

## Licensing (VERIFIED from Android.bp / NOTICE)
bionic libc/libm/linker: Apache-2.0 + BSD (OpenBSD/FreeBSD/NetBSD) + ISC + MIT; `grep GNU General bionic/*/NOTICE` finds none.
libc++/libc++abi: Apache-2.0 with LLVM exception (NDK flavor MIT/UIUC dual too). All redistributable with NOTICE files
(`sdk/licenses/bionic/*`, apex `assets/NOTICE.html.gz`). Keep NOTICE with any shipped binary. No non-redistributable part found
in the runtime apex, sdk snapshot or clang libc++. Do not redistribute `prebuilts/vndk` blobs without checking (HAL/vendor
libs there are Apache but unneeded). The apex is signed with a test key; irrelevant for use as plain files.
