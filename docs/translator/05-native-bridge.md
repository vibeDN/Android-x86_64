# 05 - NativeBridge (libarmtr_bridge): arm64-v8a JNI libraries in x86_64 app processes

Status: M0-M3 work on the device; Yougram reaches its login screen with TDLib running in the guest (see Results).

Date 2026-10-12. Code: `translator/bridge/` (+ `translator/runtime/`, shared with armtr-run). Design background: 02.

## How it works

| Piece | Where | What |
|---|---|---|
| Guest boot | `native_bridge.cc` `boot_guest()` | `initialize(isa=arm64)` maps the guest `linker64` + `/system/bin/armtr/armtr_guest` (execve-like stack/auxv, `runtime/guest_rt.cpp`) and runs the linker on the engine. The linker loads + initialises the guest libc once; `main()` of armtr_guest issues private syscall `0x4152` (READY) and the bridge parks that guest thread (never resumed). Its TLS/stack stay valid; the host thread that ran it keeps that guest CPU. Plays the role of Berberis' guest app_process64, but uses the *stock* arm64 linker64/libc (no `native_bridge` Soong variant needed). |
| dlopen/dlsym/namespaces | `nb_loadLibraryExt`, `nb_createNamespace`, ... | Call the guest linker's exported `__loader_android_dlopen_ext`, `__loader_dlsym`, `__loader_dlerror`, `__loader_android_create_namespace`, `__loader_android_link_namespaces`, `__loader_android_get_exported_namespace`, `__loader_android_init_anonymous_namespace` (found in linker64's `.dynsym`), `caller_addr` = an address in armtr_guest (=> default namespace). A `native_bridge_namespace_t*` *is* the guest `android_namespace_t*`. No host fallback (ponytail: add when an APK ships x86_64 libs under lib/arm64). |
| Guest linker config | `bridge/guest/ld.config.arm64.txt` -> `/system/etc/ld.config.arm64.txt` | One visible non-isolated `default` namespace over `/system/lib64/armtr` (mirrors Berberis' riscv64 file). libnativeloader finds no exported `system` and uses `default` as parent of the app's classloader namespace, then links it with the public library list. |
| JNI trampolines | `jni_thunks_x86_64.S` + `armtr_jni_dispatch` | 16384 pre-assembled 16-byte host stubs `mov $i,%r11d; jmp armtr_jni_common`; the common entry saves rdi..r9/xmm0-7 and calls the dispatcher with the stack-args pointer. Thunk i = `{guest fn, shorty, kind}`; the dispatcher walks the shorty (SysV: 6 int regs, 8 xmm, then stack in order; AAPCS64: x0-7, v0-7, then 8-byte stack slots), normalises sub-int types, swaps `JNIEnv*` for the proxy's guest JNIEnv (or `JavaVM*` for JNI_OnLoad, kind `kOnLoad`), runs the guest function (`armtr_rt::call`) and returns x0 or v0 in rax+xmm0. No runtime code generation, no W^X issue. Critical natives: no env/class prepended. |
| RegisterNatives | ART itself | The guest JNIEnv forwards guest fnPtrs unchanged; ART (`jni_internal.cc`) calls `isNativeBridgeFunctionPointer`/`getTrampolineForFunctionPointer` (shorty from the ArtMethod). Guest code ranges are tracked from guest `mmap/mprotect/munmap` with PROT_EXEC (+ linker and armtr_guest images). Host pointers are returned unchanged. |
| Threads | `cpu_for_thread()` | Guest-created threads: guest `clone` -> host pthread with own CPU (`guest_rt` thread hook), so they can `AttachCurrentThread` normally. Host (ART) threads entering guest code first time: new CPU + guest TLS made by calling the guest's own `pthread_create` with the clone *captured* onto the calling thread (`armtr_rt::capture_next_clone`, no thread starts; the call borrows the main guest TLS under a mutex). So `errno`, `__thread`, `pthread_self()` work per host thread. ponytail: that guest thread record (+ unused 1 MiB guest stack) leaks when the host thread exits. |
| Platform libs | `bridge/guest/gen_stubs.py` + `hostlibs/` | Guest-side proxy libraries: real arm64 `.so` files with the platform sonames (liblog, libz, libandroid, libnativewindow, libjnigraphics, libEGL, libGLESv1_CM, libGLESv2, libGLESv3, libOpenSLES), generated from `hostlibs/gen/hostlibs_tables.inc` and built with the AOSP clang (no libc dependency). Each exported function is a stub that on first call asks the bridge (syscall `0x4153`, x16 -> `{slot,"lib","sym"}`) to resolve it through `armtr_hostlibs_resolve`, caches the hostlibs proxy address in its slot and tail-jumps to it with all argument registers intact (x8/x9 preserved/clobbered). Data symbols (`SL_IID_*`) are filled by an init_array constructor (`0x4154`). Decision vs. the loader resolver: the stock guest linker resolves everything itself, so the libs must exist as files; generated stubs keep hostlibs' signature tables as the single source of truth. Unknown host functions resolve to a stub that logs and returns 0. |
| Signals | engine + runtime | `getSignalHandler` returns null. The engine's SIGSEGV/SIGBUS handler is installed with `sigaction`, which in an app process is libsigchain's: ART's fault manager runs first and passes faults outside managed code on. Guest faults go to the guest's own SIGSEGV/SIGBUS/SIGILL/SIGTRAP handler (e.g. a crash reporter in an arm64 lib) if it has one, otherwise `describe_fault` + `LOG_ALWAYS_FATAL`. Async guest handlers are delivered as in armtr-run (doc 04, "Guest signals"); a signal the host process already handles (debuggerd's, ART's) stays the host's, and one arriving on an ART thread is delivered when that thread next runs guest code (or, without a cpu, by the next guest thread passing a syscall). |

Callbacks v8 implemented: everything except `preZygoteFork` (no-op: never initialised in a zygote) and
`unloadLibrary` (no-op, like Berberis). `isSupported(path)`: aarch64 ELF check (true for bare sonames);
`isPathSupported`: contains `/lib/arm64`; `getAppEnv("arm64")`: `os_arch=aarch64 cpu_abi=arm64-v8a`.

## Build / install

Install dirs are `/system/bin/armtr/` and `/system/lib64/armtr/`, not Berberis' `<lib>/arm64`: on this tree Soong already defines
install rules for the stock `native_bridge` arm64 variants (native_bridge_support's `libnative_bridge_guest_*`, bionic's
ld-android/libdl, `native_bridge_guest_linker` -> `system/bin/arm64/linker64`, ...) because cuttlefish sets
`TARGET_NATIVE_BRIDGE_ARCH := arm64`; same-path prebuilts fail Kati with "overriding commands for target". Nothing in
ART/nativeloader/linkerconfig depends on the guest system dir (the bridge and our ld.config.arm64.txt choose it).

* Guest files: `translator/bridge/guest/build_guest.sh [OUT]` (default `/mnt/storage-adata/tmp/armtr-guest`): armtr_guest, our
  arm64 linker64/libc/libm/libdl/ld-android/libc++ (from `translator-research/sysroot-ours`, doc 04), the stub libs, ld.config.
* `scripts/sync-translator-to-aosp.sh` also runs build_guest.sh and copies the result to `device/ours/pc_x86_64/translator/guest/`
  (AOSP copy only, binaries are not in git) and copies the product mk.
* Soong (`device/ours/pc_x86_64/translator/Android.bp`): `libarmtr_bridge` (+ `libarmtr_runtime`, `libarmtr_hostlibs` from
  `translator/Android.bp`) and `armtr_guest_root` (phony) = `prebuilt_bin` (system/bin/armtr), `prebuilt_etc`
  (ld.config.arm64.txt), `prebuilt_renderscript_bitcode` (system/lib64/armtr: the only .bp-allowed prebuilt type that installs to
  lib64; `prebuilt_any` is neverallowed in .bp files, `PRODUCT_COPY_FILES` refuses ELF files).
* Product (`ours_cf_x86_64_phone.mk`, `ARMTR_ENABLE ?= true`): packages above, `ro.dalvik.vm.native.bridge=libarmtr_bridge.so`
  (overrides `?=0` from runtime_libart.mk), `ro.dalvik.vm.isa.arm64=x86_64`. Cuttlefish's BoardConfig already gives
  `TARGET_NATIVE_BRIDGE_ABI := arm64-v8a` -> `ro.product.cpu.abilist64=x86_64,arm64-v8a`. No sepolicy changes (system_lib_file /
  system_file are mappable by apps; `execmem` for the engine's TCG buffer is allowed for appdomain). No binfmt_misc.
* Image build used: `flock /mnt/storage-adata/tmp/aosp-build.lock /mnt/storage-adata/tmp/armtr-image-build.sh` (= build-aosp-own.sh
  with -j2 + memguard; log `/mnt/storage-adata/tmp/armtr-image-build.log`).

## Enable / disable

On by default: `ARMTR_ENABLE ?= true` in `device/ours/pc_x86_64/ours_cf_x86_64_phone.mk`; build with `ARMTR_ENABLE=false` to drop
the bridge, guest files and the two properties (= stock image, `ro.dalvik.vm.native.bridge=0`). After any edit under `translator/`
or the device dir: `scripts/sync-translator-to-aosp.sh` (also rebuilds the guest root). Changing an Android.bp or the product
config costs a full Soong analysis (~25-30 min, ~14 GB RAM at peak); source-only edits rebuild in a few minutes.

## Reproduce on the device

```
scripts/sync-translator-to-aosp.sh
flock /mnt/storage-adata/tmp/aosp-build.lock /mnt/storage-adata/tmp/armtr-image-build.sh   # -j2 + memguard
LCD_DENSITY=100 CMDLINE_EXTRA="console=ttyS0 androidboot.console=ttyS0" scripts/qemu/make-pc-image.sh /mnt/storage-adata/tmp/armtr-pc.img
# APK disk (test APK + yougram):
GUEST_LIBLOG=/mnt/storage-adata/tmp/armtr-guest/system/lib64/armtr/liblog.so translator/bridge/tests/build_test_lib.sh /mnt/storage-adata/tmp/armtr-apk
translator/bridge/tests/apk/build_apk.sh /mnt/storage-adata/tmp/armtr-apk/libjni_bridge_test.so /mnt/storage-adata/tmp/armtr-apk/armtr-bridge-test.apk
D=/mnt/storage-adata/tmp/armtr-apkdisk; mkdir -p $D/content; cp /mnt/storage-adata/tmp/armtr-apk/armtr-bridge-test.apk /mnt/storage-adata/apps/yougram/yougram.apk $D/content/
/usr/sbin/mke2fs -q -t ext4 -d $D/content $D/apps.img 400M
# boot (= gsi16/cf/run-pc-apps.sh with the apps disk path replaced)
/mnt/storage-adata/tmp/armtr-run-pc.sh /mnt/storage-adata/tmp/armtr-pc.img &
cd /mnt/storage-adata/gsi16/cf
python3 con.py 'su 0 mkdir -p /mnt/apk; su 0 mount -o ro /dev/block/vda /mnt/apk; pm install /mnt/apk/armtr-bridge-test.apk' 60
python3 con.py 'am start -W -n ours.test.bridge/ours.test.MainActivity' 30; grep armtr-test logcat.log     # expect ALL PASS
python3 con.py 'pm install /mnt/apk/yougram.apk; am start -W -n app.yougram/.LauncherClassic' 180; grep " armtr " logcat.log
```

## Tests

Host (glibc), fake ART: `cmake -S translator/bridge -B /mnt/storage-adata/tmp/bridge-build && cmake --build /mnt/storage-adata/tmp/bridge-build -j2`
then `translator/bridge/tests/run_tests.sh /mnt/storage-adata/tmp/bridge-build` (builds guest root + `tests/jni_bridge_test.c`):
M0 callbacks/initialize, namespaces + dlopen (by soname in an isolated classloader-like namespace), JNI_OnLoad + RegisterNatives
through ART-like trampolining, natives by name, 18-arg FP/int mix with stack spill, float/bool/short returns, Java callback
(`CallStaticLongMethod` variadic), `NewStringUTF`, guest pthread + `AttachCurrentThread`, lazy attach of host threads with private
`__thread`/errno (4 threads x 200 calls), liblog proxy lib -> host (fake) liblog with a rebuilt printf. ~0.1 s per run.

`probe_lib` (same build): loads any arm64 JNI lib through the bridge and runs JNI_OnLoad against a logging mock JNIEnv
(`PROBE_TD=1` additionally drives TDLib's JsonClient natives).

Device: `tests/apk/build_apk.sh <libjni_bridge_test.so> <out.apk>` (javac/d8/aapt2/zipalign/apksigner, no Gradle; arm64-v8a only,
`extractNativeLibs=true`); the activity runs the same natives and logs `armtr-test` PASS/FAIL lines.

## Performance

* Guest boot (linker64 + libc init under Unicorn) in `initialize`: ~85-90 ms on the QEMU/KVM guest, ~80 ms on the host.
* Host->guest JNI call through a thunk: ~3.0 us for a trivial native, ~3.8 us with errno/TLS work (host, 200k calls). Dominated by
  full register-file batches (`armtr_regs_get/set`) + `uc_emu_start` per call; cheap wins: save/restore only x0-x7/v0-v7/sp/lr/pc.
* Yougram cold start to first frame: TotalTime 959 ms (`am start -W`), vs 368 ms for the trivial test app.
* Memory: 16 MiB TCG buffer per guest CPU (main thread + every attached/guest thread), guest stacks NORESERVE.

## Not done / limits

Guest signals: limits in doc 04 (not tested on the device yet); host fallback for x86_64 libs placed in lib/arm64; `extractNativeLibs=false` (`base.apk!/lib/arm64-v8a/`, the
guest linker supports zip paths but it is untested); non-JNI `getTrampoline` (shorty == null, e.g. `ANativeActivity_onCreate`);
`long double` conversions; guest thread record cleanup on host thread exit; the guest `fork()`.

## Results (2026-10-12, QEMU PC image `/mnt/storage-adata/tmp/armtr-pc.img`)

* M0: `ro.dalvik.vm.native.bridge=libarmtr_bridge.so`, `ro.dalvik.vm.isa.arm64=x86_64`, `abilist64=x86_64,arm64-v8a`; zygote64
  loads the bridge (zygote32 logs "not found": the bridge is 64-bit only, expected). Test APK: `primaryCpuAbi=arm64-v8a`.
* M1-M3: test app (`armtr-test` tag) ALL PASS: guest boot ~90 ms, classloader namespace + dlopen from `lib/arm64`, JNI_OnLoad +
  RegisterNatives, FP/stack args, Java callback, strings, guest pthread attach, ART-thread attach, liblog proxy.
* yougram (`app.yougram`, extractNativeLibs=false): libyougram_plugin_host, libtdjsonjava (TDLib), libyg_example_hello load from
  `base.apk!/lib/arm64-v8a`, natives resolve, TDLib worker threads attach; the app reaches its phone-number login screen. No guest
  fault / unavailable host function in logcat. libyougram_voip.so is only loaded for calls (not exercised); on the host its
  JNI_OnLoad runs (probe_lib).
* TDLib does initialise in the guest: `files/tdlib/td.binlog` and `db.sqlite` (+WAL) are created and the UI shows the
  `authorizationStateWaitPhoneNumber` screen. Network was NOT exercised: the QEMU guest has no default route (`ip route` empty,
  no TCP sockets in the process), so the first real gap is still open: sockets/DNS from guest code (syscalls translate
  socket/connect/epoll; untested end-to-end on device). On the host, TDLib's client threads run and answer `getOption`.
* First failing points seen: none in the bridge. Unrelated noise: SELinux denials for RenderThread on /dev/dri/card0 (host HWUI).
  Next steps for Yougram: network in the VM, then a login; a call to exercise libyougram_voip (OpenSL ES/EGL proxies, realtime
  audio on Unicorn is expected to be too slow).
