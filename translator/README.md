# armtr: ARM64 -> x86_64 translator / NativeBridge

Design: `docs/translator/00..03`. Modules: engine (Unicorn glue), loader, syscalls, proxy, bridge, runner.

## Licensing
- `translator/` (our code): GPL-2.0-or-later (SPDX header in every file). Needed because it links Unicorn.
- Unicorn 2.1.4 (QEMU 5.0.1 core): GPL-2.0. Not vendored; fetched by `scripts/fetch-unicorn.sh`.
  `libarmtr_bridge.so` statically contains Unicorn, so the resulting binary is GPL-2.0.
- AOSP headers/libs used (libnativebridge, liblog, jni.h): Apache-2.0, compatible.
- Berberis (Apache-2.0) is read as a design reference only; no code is copied.

## Fetch Unicorn (required for both builds)
    scripts/fetch-unicorn.sh      # clones tag 2.1.4, applies the REQUIRED CF_PARALLEL patch, builds it (host)

## Build 1: host (CMake, unit tests)
    cmake -S translator/engine -B /mnt/storage-adata/tmp/engine-build && cmake --build /mnt/storage-adata/tmp/engine-build -j2
    (same for loader, syscalls, proxy, runner; ctest --test-dir <build>)

## Build 2: AOSP / Soong (bionic x86_64 device)
    scripts/sync-translator-to-aosp.sh [/mnt/storage-adata/aosp15]
It rsyncs the patched Unicorn into `external/unicorn` (+ `unicorn.Android.bp` as its Android.bp), `translator/`
into `external/armtr` and the device dir into `device/ours/pc_x86_64/translator`. Copies, not symlinks: Soong does
not descend into symlinked directories (tested). Re-run after every edit. Then:

    . build/envsetup.sh && lunch ours_cf_x86_64_phone-trunk_staging-userdebug
    m -j2 libarmtr_bridge        # -> out/target/product/vsoc_x86_64/system/lib64/libarmtr_bridge.so

Image inclusion is opt-in: `ARMTR_ENABLE=true` (see `device/ours/pc_x86_64/ours_cf_x86_64_phone.mk`,
the AOSP copy of that file must be updated too). The bridge is selected with
`ro.dalvik.vm.native.bridge=libarmtr_bridge.so`.

Files: `external/unicorn/Android.bp` (= `device/ours/pc_x86_64/translator/unicorn.Android.bp`, aarch64-softmmu
only, translated from Unicorn's CMakeLists), `translator/Android.bp` (engine/loader/syscalls/proxy static libs),
`translator/bridge/Android.bp` (sources), `device/ours/pc_x86_64/translator/Android.bp` (libarmtr_bridge,
version script exports only `NativeBridgeItf`). Unicorn's generated config headers are checked in under
`device/ours/pc_x86_64/translator/config/`.

Notes: all Soong modules are `compile_multilib: "64"` (Unicorn needs int128). Currently the M0 bridge does not call
the engine, so `--gc-sections` drops Unicorn from the .so (52 KB); it is pulled in once the bridge uses it.
First Soong analysis after Android.bp changes took 25-30 min on this host under memory pressure
(`m -j2 libarmtr_bridge` under `flock /mnt/storage-adata/tmp/aosp-build.lock`).
