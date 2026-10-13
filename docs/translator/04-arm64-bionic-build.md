# 04 - arm64 Bionic built from OUR tree (module_arm64only)

Date 2026-10-12. Result: SUCCESS. Real arm64 libc/libm/libdl/linker64/libc++/ld-android from aosp15 (android16-qpr2-release),
LIBC_V symbols present (`_Fork`, `epoll_pwait2`, `mktime_z`, `android_crash_detail_*`). Replaces Option B (prebuilt apex) of 03.

## Command (script: /mnt/storage-adata/tmp/arm64build.sh, run under flock)
```
flock /mnt/storage-adata/tmp/aosp-build.lock /mnt/storage-adata/tmp/arm64build.sh
# script body:
. /mnt/storage-adata/aosp-env.sh; export NINJA_ARGS=-j2; cd /mnt/storage-adata/aosp15
export OUT_DIR=out-arm64 TARGET_PRODUCT=module_arm64only TARGET_RELEASE=trunk_staging TARGET_BUILD_VARIANT=userdebug
nice -n 5 build/soong/soong_ui.bash --make-mode -j2 com.android.runtime libc libm libdl libc++ \
    crtbegin_dynamic crtbegin_static crtbegin_so crtend_android crtend_so
```
Gotchas found (no source edits were made):
* `OUT_DIR` must be RELATIVE inside the tree (`aosp15/out-arm64`; `/mnt/storage-adata/aosp-out-arm64` is a symlink to it).
  With an absolute OUT_DIR outside the tree Soong fails: `platform_testing/Android.bp:255 continuous_native_tests: Path is outside directory`.
* Do NOT set `TARGET_BUILD_UNBUNDLED=true`: analysis fails (`LatinIME` needs sdk variant of `libjni_latinime`).
* Soong analyses ALL Android.bp of the tree for arm64 too, so our own `external/unicorn`, `external/armtr`,
  `device/ours/pc_x86_64/translator` modules must be valid for arm64 target variants (hit: illegal flag `-w`, `visibility`
  duplicate, `include_dirs` of art/libnativebridge restricted, neverallow on `libarmtr_bridge` arm64). They were fixed by the translator agent while I
  retried; if they regress, the arm64 build breaks at analysis (consider `enabled: false`/`target: {android_arm64: {enabled:false}}` or `host_supported` only there).
* Another build (main out/) held the flock for ~35 min; mine queued behind it.

## Timings / RAM / size
* Analysis (soong_build, whole tree, cold): ~1.5 min (15:46:44 -> 15:48:30). Ninja: 14739 actions (most are host tools: protoc,
  apex tools, python archives, sepolicy; the Bionic/libc++ part is small) at -j2: 15:48:30 -> 16:06:12 = ~17.5 min. Total ~19.5 min.
* System-wide MemAvailable minimum during the run: 507 MB at 15:48 (analysis, shared box with other agents); soong peak not measured per-process
  (no /usr/bin/time on host). memguard.sh was running (pauses ninja below 800 MB); it never needed to pause.
* Out dir `aosp15/out-arm64`: 12 GB (soong 9.4 GB intermediates, target 113 MB). Sysroot: 14 MB.

## Outputs
Build tree: `/mnt/storage-adata/aosp15/out-arm64/target/product/module_arm64only/`
* `apex/com.android.runtime/bin/{linker64,linkerconfig,crash_dump64,...}`, `apex/com.android.runtime/lib64/bionic/{libc,libm,libdl}.so`,
  `lib64/{libc_malloc_debug,libc_malloc_hooks}.so`, `system/lib64/{ld-android,libc++}.so`, `system/apex/com.android.runtime.apex` (5 MB).
* On this product `system/lib64/lib{c,m,dl}.so` are symlinks to `/apex/com.android.runtime/lib64/bionic/` (as on a device).
* crt: `out-arm64/soong/.intermediates/bionic/libc/<crt>/android_arm64_armv8-a/<crt>.o`.
* NOT built: static `libc.a` / `crtbegin_static` link test (crt objects exist; libc.a is not an install target: add
  `out-arm64/soong/.intermediates/bionic/libc/libc/android_arm64_armv8-a_static/libc.a` as goal, ~5 min incl. analysis).
* `libstdc++.so` (bionic's minimal C++ runtime: operator new/delete, `__cxa_guard_*`, `__cxa_pure_virtual`; NEEDs only libc),
  2026-10-13: same script with goal `libstdc++` (~4 min, analysis cached) -> `target/product/module_arm64only/system/lib64/libstdc++.so`,
  copied to `sysroot-ours/system/lib64/` and shipped in the guest lib dir (doc 07, B2). libdl_android / libc_malloc_debug are
  not needed by apps (platform-private / only with malloc debug enabled).
* `libc++_shared.so` (arm64) is not built by Soong here (NDK flavor); copied from
  `prebuilts/clang/host/linux-x86/clang-r563880/android_libc++/ndk/aarch64/lib/libc++_shared.so`.

## Sysroot: /mnt/storage-adata/translator-research/sysroot-ours (device layout, real copies, no symlinks)
```
system/bin/linker64                                  (also apex/com.android.runtime/bin/linker64)
system/lib64/{libc,libm,libdl,ld-android,libc++,libc++_shared}.so
apex/com.android.runtime/{bin,lib64,lib64/bionic}/...   same files as device runtime apex
crt/{crtbegin_dynamic,crtbegin_static,crtbegin_so,crtend_android,crtend_so}.o
system/etc/ld.config.arm64.txt   linkerconfig/ld.config.txt (identical copy)
data/local/tmp/libjni_test.so
```
### Linker config lookup (bionic/linker/linker.cpp get_ld_config_file_path)
Order: `LD_CONFIG_FILE` env (only if linker built with `USE_LD_CONFIG_FILE` = debuggable product; NOT this build, ignored) ->
apex-specific config -> **`/system/etc/ld.config.arm64.txt`** (kLdConfigArchFilePath, ABI_STRING=arm64) -> `/linkerconfig/ld.config.txt`.
So the guest simply needs the file at `/system/etc/ld.config.arm64.txt` as seen by the guest linker (the bridge's path mapping / the
real device path - on-device ship it via PRODUCT_COPY_FILES, matches doc 02). The static config: one non-isolated default namespace,
search paths `/system/lib64`, `/apex/com.android.runtime/lib64/bionic`, `/apex/com.android.runtime/lib64`, `/data/local/tmp`; permitted `/data /system /apex`;
sections `[guest]` for executables under /system/bin, `[guest2]` for /data. Under qemu `-L` neither file is needed (linker uses built-in default config silently).
NOTE: on the real device, if the guest linker runs with path-translation, app lib dirs (`/data/app/.../lib/arm64`) must be added
to `search.paths`/`permitted.paths` (or use isolated=false, as now).

## Verification (qemu-aarch64 -cpu max -L sysroot-ours; tests linked against OUR libs/crt via script in scratchpad == build.sh with SDK stubs replaced)
Binaries: `/mnt/storage-adata/translator-research/tests-out-ours/`.
* hello_dyn: `hello bionic arm64 pid>0=1 argc=1`, rc 0
* stress_dyn: identical to golden in 03 (aes 95af22c6..., crc32 02ff48c1, ...), rc 0
* pthread_test_dyn: `counter=80000 atomic=80000 tls_bad=0 main_tls=0`, rc 0
* jni_host_dyn + libjni_test.so: `onload=0x10006 n=1 name=add sig=(II)I add(5,7)=22 add2=23`
* `llvm-nm -D system/lib64/libc.so`: `_Fork@@LIBC_V`, `epoll_pwait2@@LIBC_V`, `epoll_pwait2_64@@LIBC_V`, `mktime_z@@LIBC_V`; version nodes up to LIBC_V
  (prebuilt apex stopped at LIBC_U).
