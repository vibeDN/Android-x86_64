# 02 - NativeBridge integration design (ARM64 guest on x86_64 Android 16)

Date: 2026-10-12. Scope: everything around the CPU engine (engine = Unicorn/QEMU TCG, see 01-engine-choice.md).
Source tree: /mnt/storage-adata/aosp15 (android16-qpr2-release). All paths below are relative to it unless absolute.
Nothing here was built; claims marked (verify) need a check at the named milestone.

## 0. Key findings (read this first)

1. **The platform side is already wired for arm64-v8a on x86_64.** `device/google/cuttlefish/vsoc_x86_64/BoardConfig.mk` already sets
   `TARGET_NATIVE_BRIDGE_ARCH := arm64`, `TARGET_NATIVE_BRIDGE_ABI := arm64-v8a` (+ arm 32-bit 2nd arch). The built image
   (`out/target/product/vsoc_x86_64/system/build.prop`) already has `ro.system.product.cpu.abilist64=x86_64,arm64-v8a`.
   What is missing is only: `ro.dalvik.vm.native.bridge=<our lib>` (currently `=0`, set by `build/make/target/product/runtime_libart.mk:103`),
   `ro.dalvik.vm.isa.arm64=x86_64`, our bridge library, and the guest (arm64) system libraries.
2. **Berberis (riscv64 guest) is the right template and ~60% reusable**, because its design is "run a *real* guest Bionic
   (linker64 + libc.so + libm + libdl ...) built by the AOSP build for the guest arch, and intercept selected libc/NDK symbols
   with host-side proxy trampolines". Only the translation core (decoder/interpreter/JIT, ~half of the repo) is riscv64-specific
   and gets replaced by the engine. Remnants of an arm64 *guest* exist in-tree (`guest_state/arm64`, `guest_abi/arm64`,
   `calling_conventions`, `native_bridge_support/android_api/*/stubs_arm64.*`, `*/proxy/api_arm64.json`,
   `*/proxy/trampolines_arm64_to_x86_64-inl.h`, `vdso/vdso_arm64.S`, `linker/init_static_tls_arm64.S`, `kernel_api/tools/kernel_api_arm64.json`).
   `guest_state/arm64/.../guest_state_cpu_state.h` even has `#if defined(__x86_64__)` flag masks, i.e. it was an arm64->x86_64 guest state.
3. **arm64 and riscv64 share the asm-generic syscall ABI** (same syscall numbers, same `struct stat`, same `kernel_sigaction`, ...).
   The riscv64->x86_64 syscall translation in `kernel_api/riscv64/` and `guest_os_primitives/riscv64/gen_syscall_numbers_x86_64.cc`
   is therefore ~90% valid for arm64 as is; only arch-specific calls (`riscv_hwprobe`, `clone` arg order, signal frame) differ.
4. **Target apps**: yougram's `libtdjsonjava.so` imports 214 symbols, NEEDED = libdl, libz, liblog, libm, libc (static libc++).
   `libyougram_voip.so` (50 MB, 16 KiB-aligned LOAD segments, built with NDK r26-ish clang 18) imports 373 symbols, NEEDED = libjnigraphics, liblog,
   libz, libGLESv2, libEGL, libandroid, libOpenSLES, libm, libdl, libc. So we need guest `libz`, `liblog`, `libOpenSLES`, `libEGL`, `libGLESv2`,
   `libandroid`, `libjnigraphics` (proxy libs for the last five; Berberis already has proxy definitions for them).
   Uses `pthread_*` heavily, `epoll`, sockets, `syscall()`, `sigaction/sigaltstack/siglongjmp`, `dl_iterate_phdr`, `getauxval`, `__system_property_get`,
   `sched_getaffinity`, `__sF`. No `libc++_shared.so` is needed (statically linked), but keep it in the guest lib set for other apps.

## 1. How the system selects the ABI and routes to the bridge (verified in source)

| Step | Where | What happens |
|---|---|---|
| ABI list | `build/make/core/board_config.mk:350-400` | `TARGET_NATIVE_BRIDGE_ABI` is appended after the native ABIs: `ro.product.cpu.abilist64=x86_64,arm64-v8a`. |
| "emulated?" test | `frameworks/base/services/core/java/com/android/server/pm/PackageAbiHelperImpl.java:316-326` | An ABI is "natively supported" iff `ro.dalvik.vm.isa.<isa>` is empty. With `ro.dalvik.vm.isa.arm64=x86_64` arm64 is considered emulated: an APK with only `lib/arm64-v8a` gets primary ABI `arm64-v8a`; native-first policy still prefers x86_64 libs when the APK has both. |
| Lib extraction | PackageManager `NativeLibraryHelper` | Extracts to `<app>/lib/arm64/` (isa dir name, not abi dir) when `extractNativeLibs=true`; with `extractNativeLibs=false` (AGP default) libs are mapped from `base.apk!/lib/arm64-v8a/` directly. The guest linker handles `apk!/` paths itself (bionic zip support), but offsets must satisfy the guest mmap rules (verify at M4). |
| Zygote choice | `ZygoteProcess` | Uses `ro.product.cpu.abilist64`; arm64-v8a is in it, so the 64-bit zygote (x86_64 `app_process64`) forks the app. No 32-bit arm needed for yougram (it also ships armeabi-v7a, which we ignore). |
| Bridge load | `frameworks/base/core/jni/AndroidRuntime.cpp:1123-1134` | zygote passes `-XX:NativeBridge=<ro.dalvik.vm.native.bridge>` to ART. Name must match `[a-zA-Z][a-zA-Z0-9._-]*` (`art/libnativebridge/native_bridge.cc` `NativeBridgeNameAcceptable`). `LoadNativeBridge` dlopens it from the zygote's namespace and looks up the `NativeBridgeItf` symbol. |
| Per-app init | `com_android_internal_os_Zygote.cpp:1928-1995` | `NeedsNativeBridge(instruction_set)` is true when the app ISA ("arm64") != `ABI_STRING` ("x86_64"): `PreInitializeNativeBridge(app_data_dir, isa)` (mounts emulated storage differently) and later `InitializeNativeBridge` -> `callbacks->initialize(runtime_cbs, private_dir, isa)` and `getAppEnv(isa)` (used for `os.arch`). |
| Library load | `art/libnativeloader/native_loader.cpp:389,504-510` + `library_namespaces.cpp` | `NativeBridgeIsPathSupported(path)` decides if the *path* is for the bridged ISA, `NativeBridgeIsSupported` fallback, then `NativeBridgeLoadLibrary(Ext)`. Namespaces: libnativeloader asks the bridge to `createNamespace/linkNamespaces/getExportedNamespace` mirroring the host ones (system, sphal, com_android_i18n, ...). |
| JNI binding | ART `JniMethod / RegisterNatives` | For every native method ART calls `getTrampolineWithJNICallType(handle, name, shorty, len, type)` (or `getTrampolineForFunctionPointer` for `RegisterNatives`), then calls the returned *host* function pointer. `JNI_OnLoad` goes through `getTrampoline...(handle,"JNI_OnLoad",nullptr,..)`. ART uses `isNativeBridgeFunctionPointer` (v8) to detect guest pointers. |

NativeBridgeCallbacks versions (`art/libnativebridge/include/nativebridge/native_bridge.h`): v1 initialize/loadLibrary/getTrampoline/isSupported/getAppEnv;
v2 +isCompatibleWith/getSignalHandler; v3 +unloadLibrary/getError/isPathSupported/(unused)initAnonymousNamespace/createNamespace/linkNamespaces/loadLibraryExt;
v4 +getVendorNamespace; v5 +getExportedNamespace; v6 +preZygoteFork; v7 +getTrampolineWithJNICallType/getTrampolineForFunctionPointer; v8 +isNativeBridgeFunctionPointer.
The struct order of the v8 table is exactly the one in Berberis `native_bridge/native_bridge.cc` (bottom). Runtime callbacks given to `initialize`: `getMethodShorty`, `getNativeMethodCount`, `getNativeMethods`.

### Required product changes (device/ours/pc_x86_64)

`ours_cf_x86_64_phone.mk` inherits `device/google/cuttlefish/vsoc_x86_64/phone/aosp_cf.mk`, which already gives `TARGET_NATIVE_BRIDGE_*`. Add:

```make
# --- ARM64 translator ---
PRODUCT_PACKAGES += libours_nb ours_nb_guest_runtime      # bridge + guest lib/linker/proxy bundle (see 3.)
PRODUCT_SYSTEM_PROPERTIES += \
    ro.dalvik.vm.native.bridge=libours_nb.so \
    ro.dalvik.vm.isa.arm64=x86_64 \
    ro.enable.native.bridge.exec=1        # only if binfmt_misc exec of arm64 ELFs is wanted (dev tooling, M1/M2 tests)
PRODUCT_COPY_FILES += \
    device/ours/pc_x86_64/nb/ld.config.arm64.txt:$(TARGET_COPY_OUT_SYSTEM)/etc/ld.config.arm64.txt \
    device/ours/pc_x86_64/nb/ours_nb.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/ours_nb.rc \
    device/ours/pc_x86_64/nb/arm64_dyn:$(TARGET_COPY_OUT_SYSTEM)/etc/binfmt_misc/arm64_dyn \
    device/ours/pc_x86_64/nb/arm64_exe:$(TARGET_COPY_OUT_SYSTEM)/etc/binfmt_misc/arm64_exe
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += system/lib64/arm64/% system/bin/arm64/% ...   # as Berberis' BERBERIS_DISTRIBUTION_ARTIFACTS_RISCV64
PRODUCT_SOONG_NAMESPACES += frameworks/libs/native_bridge_support/android_api/libc
```
Also `include frameworks/libs/native_bridge_support/native_bridge_support.mk` and add `$(NATIVE_BRIDGE_PRODUCT_PACKAGES)` (this is what builds the
`*.native_bridge` guest variants: `libnative_bridge_vdso`, `native_bridge_guest_app_process`, `native_bridge_guest_linker`, original guest libs libz, liblog, libcutils,
libutils, libdl.bootstrap, libicu.bootstrap, libcrypto/ssl, libsqlite, libstdc++ ...; and "modified" guest libs `libnative_bridge_guest_{libc,libm,libandroid,libEGL,libGLESv2,libjnigraphics,libOpenSLES,...}`
whose bodies are interceptable stubs). Notes:
* The property has to be in the **system** partition (`PRODUCT_SYSTEM_PROPERTIES`); sepolicy: `system/sepolicy/private/property_contexts:540` already labels `ro.dalvik.vm.native.bridge` as `dalvik_config_prop`. `ro.dalvik.vm.isa.*` falls under the same `ro.dalvik.vm.` prefix rule (verify with `ls -Z` / `getprop` at M0).
* Guest libs live in `/system/lib64/arm64/` (Berberis: `/system/lib64/riscv64/`), guest executables in `/system/bin/arm64/{linker64,app_process64}`; guest linker config `/system/etc/ld.config.arm64.txt` (copy `frameworks/libs/binary_translation/prebuilt/system/etc/ld.config.riscv64.txt`, s/riscv64/arm64/).
* binfmt_misc magic for an aarch64 ELF: `\x7fELF\x02\x01\x01\x00...\x02\x00\xb7\x00` (EM_AARCH64=0xb7; exe=ET_EXEC 0x02, dyn=ET_DYN 0x03). Register in `ours_nb.rc` like `prebuilt/system/etc/init/berberis.rc`. Runner = `ours_nb_program_runner` (M1/M2 test tool, section 6). Not needed for apps.
* **SELinux**: app domains already have `allow appdomain self:process execmem` (`system/sepolicy/private/app.te:204`), so an engine JIT (anonymous RWX) works in apps. Guest libs under `system_lib_file` (execute ok). App-private libs are `apk_data_file`/`app_data_file`; Android 10+ forbids `execute` on app_data_file for targetSdk>=29 *exec()*, but `mmap(PROT_EXEC)` of `apk_data_file` libs in `/data/app` is permitted (same as dlopen). Anything the bridge itself needs beyond that (e.g. reading `/system/lib64/arm64/*` from the `zygote` and app domains): `system_lib_file` is world-readable. For the optional translated-code cache dir (`<app>/code_cache`, passed to `initialize`) no policy change. Expect zero new sepolicy for M0-M4; only the binfmt dev tooling needs `shell`/`su` domains and `binfmt_miscfs` mount perms (already exists for init).
* **seccomp**: apps use the *host* x86_64 filter (`bionic/libc/SECCOMP_ALLOWLIST_APP.TXT` + COMMON/PRIORITY) installed by zygote. Guest syscalls are executed by the bridge as host x86_64 syscalls, so the filter applies to the *translated* call. Only host-allowed syscalls can be emitted; arm64 apps must never see the arm64 filter. Danger: syscalls an arm64 app does that are in the x86_64 blocklist (rare) fail with SIGSYS in the bridge: handle `SIGSYS` -> return `-EPERM/ENOSYS` to guest. Also the engine itself must avoid forbidden calls (`memfd_create`, `mprotect(PROT_EXEC)` are allowed in app filter; check `userfaultfd`, `process_vm_*` not used).
* `ro.dalvik.vm.isa.arm64=x86_64` also tells ART/dex2oat to compile the app's *Java* code for x86_64 (the isa mapping), which is what we want.

## 2. How Berberis implements the callbacks (what we copy)

Source: `frameworks/libs/binary_translation/native_bridge/native_bridge.cc` (668 lines), arch part `native_bridge/riscv64/native_bridge.cc` (kGuestIsa, kSupportedLibraryPathSubstring="/lib/riscv64", empty RuntimeValues).

| Callback | Berberis behaviour | Ours (arm64) |
|---|---|---|
| initialize | Check isa == kGuestIsa, store runtime callbacks, derive app package from `<pkg>/code_cache`, `InitBerberis()`, `GuestLoader::StartAppProcessInNewThread()` (loads guest app_process64, vdso, linker64 via TinyLoader; starts a thread which executes guest linker -> libc init -> `main()` of guest app_process that calls `native_bridge_post_init` and `pthread_exit`), registers `ANativeActivity_onCreate` wrapper, `InitializeJNI()`. | same; "guest process" = arm64 linker64 + app_process64 under the engine. |
| loadLibrary / loadLibraryExt | First call runs `FinalizeInit` (copy `__progname` into guest libc, protect libc mappings in `GuestMapShadow`). Then `GuestLoader::DlOpenExt` (= call guest linker's `__loader_android_dlopen_ext` via `GuestCall`); if it fails, **fall back to host `android_dlopen_ext`** (host libs placed in an arm64 path, e.g. x86_64 libs - handled by `host_libraries_` set). | same. Host fallback matters for APKs that ship x86_64 libs in `lib/arm64`. |
| getTrampoline(WithJNICallType) | `DlSym` on guest handle -> if `shorty!=null` `WrapGuestJNIFunction(addr, shorty, name, has_jnienv_and_jobject)` which builds a *host-callable* thunk that marshals host (SysV) args to guest AAPCS regs and runs `RunGuestCall`; for critical natives `has_jnienv_and_jobject=false`. `JNI_OnLoad` has a registered known-wrapper (`"ipp"`, replaces JavaVM* with a guest-visible JavaVM). | same logic; arm64 AAPCS from `guest_abi/arm64` + `calling_conventions`. |
| getTrampolineForFunctionPointer / isNativeBridgeFunctionPointer | `GuestMapShadow::IsExecutable(addr)` decides guest-vs-host pointer. | same (needs our own shadow: guest-mapped exec ranges, or "is it inside a mapping created by guest linker"). |
| isSupported | `true`. isPathSupported: `strstr(path, "/lib/riscv64")`. | `"/lib/arm64"` (matches `lib/arm64` and `lib/arm64-v8a`). |
| getAppEnv | all-null struct (non-null pointer is essential: null disables the bridge). | non-null; `os_arch="aarch64"`, `cpu_abi="arm64-v8a"`. |
| isCompatibleWith | `2..8` | same. |
| getSignalHandler | `nullptr` (Berberis installs guest-signal machinery itself). | `nullptr` at first; see 5.4. |
| createNamespace / linkNamespaces / getExportedNamespace / initAnonymousNamespace | Keeps a *pair* `{guest_namespace, host_namespace}` per NB namespace: creates both in the guest linker (`__loader_android_create_namespace` called in the guest via GuestCall) **and** in the host linker (`android_create_namespace`), so a library the guest cannot load can fall back to the host. `GuestLoader` finds the guest linker's exported entry points through `linker_callbacks.cc` (symbols resolved in the loaded linker64 ELF). | same. |
| getVendorNamespace | `GetExportedNamespace("sphal")` | same |
| preZygoteFork | close guest `liblog` fds (`__android_log_close`) + `PreZygoteForkUnsafe` | same (app-zygote only). |
| unloadLibrary | no-op returning 0 | same |

## 3. Components

```
 zygote / app process (x86_64, bionic)
 +---------------------------------------------------------------------------------------------+
 | ART  --NativeBridgeItf-->  libours_nb.so (A)  Apache/GPL glue, dlopen'd by ART                |
 |                              |                                                              |
 |   (A) native_bridge.cc  callbacks            (B) guest_loader  -- TinyLoader (ELF mapper)    |
 |   (C) jni bridge: JNIEnv/JavaVM guest vtables, shorty marshalling                            |
 |   (D) guest_os: GuestThread, TLS, signals, clone/pthread_create, GuestMapShadow              |
 |   (E) kernel_api: arm64->x86_64 syscall + struct translation                                 |
 |   (F) proxy_loader + libours_nb_proxy_<lib>.so: host implementations of guest libs' APIs     |
 |   (G) engine.h  ----------------------------------->  libours_engine.so (GPL: Unicorn/TCG)   |
 +---------------------------------------------------------------------------------------------+
 guest address space == host address space (identity mapped)
 guest image (all arm64, built by AOSP as *.native_bridge variants):
   /system/bin/arm64/linker64, app_process64  /system/lib64/arm64/{libc,libm,libdl,liblog,libz,libc++,libstdc++,libandroid,libEGL,libGLESv2,libOpenSLES,libjnigraphics,libnative_bridge_vdso,...}.so
   app: /data/app/.../lib/arm64/*.so (or base.apk!/lib/arm64-v8a/*.so)
```

| # | Component | Source | Notes |
|---|---|---|---|
| A | `libours_nb.so` (NativeBridgeItf) | new, skeleton in `translator/bridge/native_bridge.cc` | thin; delegates to B..D. |
| B | Guest ELF loader | **reuse** `tiny_loader/` (generic ELF64 mapper, no arch check, `MmapForGuest` hooks) + `guest_loader/guest_loader.cc` | loads only 3 files: `linker64`, `app_process64`, `libnative_bridge_vdso.so`; everything else is loaded by the *guest linker* running on the engine. Needs `InitKernelArgs` (auxv) rewritten for arm64 (AT_HWCAP/AT_HWCAP2, AT_PLATFORM "aarch64", AT_MINSIGSTKSZ, AT_SYSINFO_EHDR vdso). |
| C | JNI bridge | **reuse** `jni/` (api.json driven generator `gen_jni_trampolines.py`, `jni_trampolines.cc`, `guest_jni_trampolines.cc`) | arch-neutral except the `GuestCall` arg-adding in `guest_jni_trampolines.cc` (`BERBERIS_GUEST_LP64` path = what we need). |
| D | Guest threads/OS | rewrite arch parts of `guest_os_primitives/` | see 5. |
| E | Syscall translation | adapt `kernel_api/` | see 5.2. |
| F | Proxy libraries | **reuse** `native_bridge_support/android_api/*` (+ `proxy_loader/`) | see 4. |
| G | Engine adapter | new `translator/bridge/engine.h` + engine glue | replaces `runtime/`, `interpreter/`, `lite_translator/`, `heavy_optimizer/`, ... |
| H | Guest vdso | **reuse** `android_api/vdso/vdso_arm64.S` | provides `native_bridge_trace/intercept_symbol/post_init/call_guest`. On Berberis the host recognises their addresses; with Unicorn use the trampoline mechanism of 4.3 (replace the `ldr x3,=0; blr x3` stubs by `svc` stubs). |
| I | Build glue | `Android.bp` for A..G with `soong_config` on `translation_arch`; product package list like `berberis_config.mk` | |

### 3.1 Guest linker / libc strategy (decision)

**Use a real arm64 Bionic (AOSP source, `native_bridge` variant) + the real arm64 `linker64`**, exactly like Berberis. Alternatives rejected:
* Own mini-linker + HLE libc (all libc symbols implemented as host trampolines): breaks stack protector (`tpidr_el0+0x28` canary), TLS (`__errno`, `thread_local`, TLSDESC in clang 18 arm64 output), `__sF`, locale, stdio, `dl_iterate_phdr` (needed for C++ exceptions/unwinding `.eh_frame`), `fork` semantics ... and every new libc symbol is a bug. Reserve as no-go.
* Prebuilt arm64 Bionic from a stock arm64 AOSP: would work but version-skew vs Berberis' hooks (`__native_bridge_config_static_tls`, `__native_bridge_get_host_pthread`, `__BIONIC_WEAK_FOR_NATIVE_BRIDGE` markers already live in our `bionic/`), so build from our tree.

How it is built: `native_bridge_support.mk` -> `.native_bridge` modules; `libnative_bridge_guest_libc` (`frameworks/libs/native_bridge_support/android_api/libc/Android.bp`) is bionic's libc with `nocrt`, interceptable stubs (`stubs_arm64.cpp`: `DEFINE_INTERCEPTABLE_STUB_FUNCTION(__system_property_get)`, pthread cleanup, `_longjmp`, `__clone_for_fork`, ...) and `-z muldefs`. The guest linker (`native_bridge_guest_linker`, `android_api/linker`) is bionic's linker compiled with `-D__ANDROID_APEX__=native_bridge_apex` (skips `bootstrap` dir), `linker_tls.cpp` (reports static TLS layout to the host via `__native_bridge_config_static_tls`), `linker_translate_path.cpp`, `__libc_init_main_thread.cpp` (host provides pthread_self) and `liblinker_debuggerd_stub`. **The Soong `native_bridge` target arch for arm64 on an x86_64 product is declared in cuttlefish's BoardConfig, but nobody has built the arm64 variants in our tree yet: first task of M0/M1 is `m libc.native_bridge linker.native_bridge ...` (verify; one build, <1 h incremental but heavy on the 15 GB build box, use memguard).**

How guest libc reaches the host (two mechanisms, as in Berberis):
1. **Raw syscalls** (`svc #0` in bionic's syscall stubs, in the linker, in static libs inside app .so such as OpenSSL/libyuv/WebRTC): engine traps SVC -> `kernel_api` translates to a host x86_64 syscall (5.2).
2. **Intercepted symbols**: when the guest linker resolves a symbol in `libc.so`/`libm.so`/`libdl`/`libandroid`/`liblog`/... that is declared as an interceptable stub, it calls `native_bridge_intercept_symbol(addr, lib, name)` (vdso). `proxy_loader/proxy_loader.cc` dlopens `libours_nb_proxy_<lib>.so` (prefix replaces Berberis' `libberberis_proxy_`), runs its `InitProxyLibrary`, and `ProxyLibraryBuilder::InterceptSymbol` binds the guest address of the stub to a **trampoline** (`MakeTrampolineCallable(guest_addr, marshal_and_call, host_thunk)`): guest `bl malloc` lands on the stub, engine sees the registered address, marshals X0..X7/V0..V7 to host SysV args via generated `trampolines_arm64_to_x86_64-inl.h`, calls the host function, writes the result to X0/V0, resumes at LR. This is how `malloc/free` (so Java/host/guest share one heap), `pthread_*`, `__system_property_get`, `liblog`, `libandroid`, EGL/GLES, OpenSL, `libjnigraphics`, `ASurfaceTexture`, etc. work.

## 4. Proxy libraries and host function exposure

### 4.1 What exists
Per-lib directory `frameworks/libs/native_bridge_support/android_api/<lib>/` with `api_arm64.json` (symbol list+signatures), `custom_trampolines_arm64_to_x86_64.json`, generated `trampolines_arm64_to_x86_64-inl.h`, `stubs_arm64.cc` (guest-side stub .so), and `frameworks/libs/binary_translation/android_api/<lib>/` (host-side proxy .so: `libberberis_proxy_<lib>`). Generators: `gen_proxy_libraries.py`, `gen_known_trampolines.py`, `check_proxy_libraries.py`. Libs: libc, libm, libandroid, libandroid_runtime, libEGL, libGLESv1_CM/2/3, libjnigraphics, libOpenSLES, libOpenMAXAL, libaaudio, libamidi, libbinder_ndk, libcamera2ndk, libmediandk, libnativehelper, libnativewindow, libneuralnetworks, libvulkan, libwebviewchromium_plat_support. Pure-guest (rebuilt for arm64 as is): liblog (it talks to `logd` socket via the translated syscalls; works, but proxying `__android_log_write` to host liblog is cheaper and avoids the `/dev/socket/logdw` SELinux path - keep as guest unless broken), libz, libcutils, libutils, libcrypto/ssl, libsqlite, libicu.
`libc` proxy specifics (`libc/proxy/`): `pthread_translation.cc` (guest `pthread_create` -> host thread + `GuestThread`), `malloc_translation.cc`, `setjmp_thunks.cc`, `unistd_thunks.cc` (`fork/exec` family), `cxa_trampolines.cc` (`__cxa_atexit/__cxa_thread_atexit_impl` run guest dtors on the right thread), `system_properties_trampolines.cc`, `libc_translation.cc` (long double `strtold` etc. - arm64 `long double` is IEEE binary128, x86_64's is x87 80-bit: **needs real conversions**, `long_double.h`; yougram uses `strtold_l`, `wcstold`).

### 4.2 JNIEnv / JavaVM
Guest code gets a *guest-visible* function table whose entries are trampolines to host `JNINativeInterface` functions: `ToGuestJNIEnv(host_env)` / `ToGuestJavaVM` (`jni/jni_trampolines.cc`, `WrapHostFunctionImpl(vtable[i], DoTrampoline_*)`, generated from `jni/api.json`). Special cases that are *not* plain calls: `RegisterNatives` (wrap each guest fnPtr with `WrapGuestJNIFunction` using the shorty from the runtime callback), `GetJavaVM`, `Call*MethodV/A` (va_list in guest AAPCS64 va_list layout - struct with __stack,__gr_top,__vr_top,__gr_offs,__vr_offs, unlike x86_64's; `GuestVAListParams` in `guest_abi/arm64`), `NewObject`, `GetEnv`/`AttachCurrentThread` (give out the guest-JNIEnv of the current thread; per-thread map + exit listener `RegisterGuestThreadExitListener`), `JNI_OnLoad(JavaVM*, void*)`.

### 4.3 Trampoline mechanism with a non-JIT engine (new, engine-specific)
Berberis registers "this guest address is a host callable" in its own translation cache. With Unicorn:
* Allocate a **thunk page** in guest memory (RX). Thunk *i* = `mov w16,#i ; svc #0x4e42 ; ret` (12 bytes). `MakeTrampolineCallable(addr,...)` for symbols living in already-mapped guest libs instead overwrites the 1st instruction of the interceptable stub with `b <thunk i>` (stubs are 16-byte aligned, 4-instruction bodies in `interceptable_functions.h` - verify) so no code-hook is needed on the hot path.
* Engine SVC hook: imm `0x4e42` -> `trampoline_table[x16]` -> `marshal_and_call(callee, ThreadState*)`; any other imm = Linux syscall (`x8` = number).
* Host->guest calls (`GuestCall.RunVoid/RunResInt64`, JNI wrappers) = `engine_run(entry,args[8])` with LR = `native_bridge_call_guest` (vdso), i.e. return address points to a thunk that makes the engine stop (`uc_emu_stop` or `until` address).

## 5. Guest/host OS layer

### 5.1 Memory
* Identity mapping contract (engine side): one `uc_mem_map_ptr(0x10000, 1<<47... , host=same)` covering the whole user VA; guest pointers == host pointers; the loader's `MmapForGuest` is plain `mmap` (Berberis `sys_mman_emulation.cc` additionally records exec ranges in `GuestMapShadow` and **protects host-critical mappings** (libc) from guest `munmap/mprotect`; keep, it fixed real app crashes b/218772975).
* Host bionic page size is 4 KiB; arm64 libs are 16 KiB aligned - fine (alignment is a multiple). `getpagesize()` in guest returns AT_PAGESZ=4096. Apps linking with `-z max-page-size=16384` and a 64 KiB VA hole assumption work.
* Executable code written by the guest (JIT inside app, e.g. V8/WebView-style libs) requires engine TB invalidation on `ic ivau` / `dc cvau` / `mprotect(+X)` - engine contract `engine_flush_icache(addr,len)`. Translated-code cache isn't shared across processes; zygote can't pre-warm (app-process starts after fork).
* Memory tagging: strip TBI/MTE: Bionic 16 for arm64 may enable heap tagging via `prctl(PR_SET_TAGGED_ADDR_CTRL)` -> report EINVAL; scudo `android_mallopt` -> disable (`libnative_bridge_guest_libc` uses `libc_native_allocator_defaults` + proxy `malloc` so scudo isn't used for the app heap anyway).

### 5.2 Syscalls (aarch64 -> x86_64)
Pipeline already exists: `kernel_api/tools/*.py` take per-arch DWARF-extracted JSON (`kernel_api_arm64.json`, `kernel_api_x86_64.json` both in tree) and generate `gen_syscall_emulation_<guest>_to_<host>-inl.h` + `gen_syscall_numbers_<host>.cc`. Run the generators for guest=arm64 (checked-in riscv64_to_x86_64 outputs are the reference; arm64 and riscv64 numbers are identical for everything except riscv-specific `__NR_riscv_flush_icache` 259 (arm64 has no 259 ... verify) and the arm64-only `__NR_arch_specific_syscall`).
Hand-written parts (copy of `kernel_api/riscv64/syscall_emulation.cc` and friends, tweak):
* `fstat/newfstatat` (`struct stat` asm-generic 128 B vs x86_64 144 B): `ConvertHostStatToGuestArch` - identical to riscv64 (also asm-generic). `statx`, `statfs` identical layout.
* `epoll_event` is packed (12 B) on x86_64, 16 B on arm64: reuse `riscv64/epoll_emulation.cc`. arm64 has `epoll_pwait` only (no `epoll_wait`): host `epoll_pwait` ok.
* `rt_sigaction/rt_sigprocmask/rt_sigreturn/sigaltstack`: guest handlers are **never** installed on the host directly (host handler = bridge trampoline), see 5.4.
* `clone/clone3`: arm64 argument order differs (flags, stack, ptid, **tls, ctid**) from x86_64; threads are created by the proxy `pthread_create` -> host thread, raw `clone(CLONE_VM|...)` (used by bionic `fork`/`posix_spawn` and some apps) emulated by `guest_thread_clone.cc` (thread case) and `fork` thunk (`__clone_for_fork` intercepted).
* `open/openat` path translation (`open_emulation.cc`: `/proc/self/exe`, `/proc/self/maps` filtering, `/proc/cpuinfo` -> synthesize an aarch64 cpuinfo with `Features : fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics ...` because apps (WebRTC/libyuv/OpenSSL) parse it or use `getauxval(AT_HWCAP)` instead), `/sys/devices/system/cpu/*`.
* `ioctl`: pass-through with whitelisted struct conversions (TCGETS (termios differs), binder (guest binder via libbinder_ndk proxy, not raw ioctl), ashmem/dmabuf, FIONREAD, SIOCGIF*). Everything else: pass through with TRACE.
* `mmap/mremap/mprotect/munmap/madvise/brk`: same ABI; hooked for `GuestMapShadow` and for engine memory-permission sync.
* `prctl(PR_SET_VMA...)`, `PR_PAC_*`/`PR_SET_TAGGED_ADDR_CTRL`/`PR_SVE_*`: EINVAL. `getrandom`, `futex`, `ppoll`, `pselect6`, `eventfd2`, sockets: direct.
* `syscall()` libc wrapper called by app code goes the same SVC path.
* `AT_HWCAP` value: ASIMD|FP|AES|PMULL|SHA1|SHA2|CRC32|ATOMICS; **omit HWCAP_CPUID** so bionic ifuncs do not `mrs MIDR_EL1` (engine would need to emulate); report no SVE/MTE/PAC unless the engine supports them (Unicorn 5.0 base: check `ARM_FEATURE_V8_*`, 01-engine-choice risk 1).

### 5.3 Threads and TLS
* Every guest thread is a host `pthread` (proxy `pthread_create` -> `CreateNewGuestThread`: `GuestThread::CreatePthread(stack,...)`, host thread runs `RunGuestThread` -> `engine_run(func,arg)`), so futexes, signals, `gettid`, priority, affinity all stay native. One engine context (`uc_engine`) per host thread, created lazily in `GetCurrentGuestThread()` (this also covers **foreign host threads** entering guest code through JNI: Berberis `GuestThread::CreateForeign` allocates a guest stack + static-TLS block using the layout reported by `__native_bridge_config_static_tls` and `init_static_tls_arm64.S`).
* arm64 TLS: `TPIDR_EL0` -> Bionic `bionic_tcb`; engine contract exposes get/set `tpidr_el0` (plus `tpidrro_el0` unused). `ThreadState.tls` shadows it. Stack-protector canary at `[tp,#0x28]` and shadow-call-stack `x18` (Bionic arm64 uses SCS: allocate it in `GuestThread` as Berberis does via `SetShadowCallStackPointer`).
* Host-side Android TLS (`__get_tls()` slots used by ART/libc on host) is untouched: the guest never sees host TLS.
* Atomics: LDXR/STXR/LSE across engines rely on the TCG helpers using host atomics; **test before M4** (voip lib is atomics-heavy).
* `pthread_exit` of the guest main thread after `native_bridge_post_init` must keep the guest stack (argv/auxv live there) - `DisallowStackUnmap`.
* Thread exit: engine context destroyed on `pthread` key destructor; JNI env mapping removed (`RegisterGuestThreadExitListener`).

### 5.4 Signals and faults
* ART/bionic host already owns SIGSEGV (null checks, stack overflow, `debuggerd`), SIGBUS, SIGSYS. Bridge installs nothing at `getSignalHandler`; instead (as Berberis `guest_os_primitives/guest_signal_handling.cc`) guest `rt_sigaction` records the guest handler and installs a *host* handler for the signal that (a) if the signal interrupts guest execution, builds an arm64 `rt_sigframe` (siginfo 128 B + `ucontext`: `uc_flags,uc_link,uc_stack,uc_sigmask(+pad),__unused[...]; sigcontext{fault_address,regs[31],sp,pc,pstate}+__reserved{fpsimd_context{head,fpsr,fpcr,vregs[32]}}`) on the guest (alt)stack and runs the guest handler through the engine; the handler's `ret` goes to the sigreturn thunk (`mov x8,#139; svc 0`) which restores state; (b) otherwise chains to the previous host handler. `guest_signal.cc`/`guest_context_arch.h` are riscv64 specific -> rewrite (est. 3-4 days). Pending-signal protection while the engine is inside a TB: `ScopedPendingSignals` from Berberis; with Unicorn deliver signals only at `uc_emu_start` boundaries/SVC hooks and use `uc_emu_stop` from the host handler for prompt delivery.
* Guest memory fault inside the engine (unmapped page) surfaces as `UC_ERR_*_UNMAPPED`/host SIGSEGV in a helper (01-engine-choice risk 3): convert to guest SIGSEGV with `si_addr`; for host-side SIGSEGV while *in engine code*, ART's handler must not see it (use `sigsetjmp` around `uc_emu_start`, a thread-local "in-engine" flag, chain otherwise). Handler order: ART registers via `sigchain`; the bridge's SIGSEGV hook goes through libsigchain's `AddSpecialSignalHandlerFn` equivalent - **verify** whether apps (not zygote) can add special handlers (ART does it itself for the runtime; our bridge is loaded inside the runtime so link `libsigchain` symbols `AddSpecialSignalHandlerFn`; else chain manually).
* `abort()` in guest: `tgkill(SIGABRT)` -> host debuggerd gives an x86_64 tombstone of the *bridge* thread (guest PC is not decodable). Add a guest-PC/backtrace line in `crash_reporter.h` equivalent (low priority, but essential for debugging yougram).

### 5.5 Other
* `fork()/exec*` from guest: `execve` of an arm64 ELF re-enters the bridge via binfmt_misc (`exec_emulation.cc` demangles env) - optional; for apps, Process.exec is rare.
* `dl_iterate_phdr`, `__cxa_*`, C++ exceptions: handled inside guest libc/libunwind/libc++, no host involvement, only needs `AT_PHDR`, `.eh_frame_hdr` mapped (normal).
* `getauxval`, `AT_SYSINFO_EHDR` vdso: `libnative_bridge_vdso.so` has no `__kernel_clock_gettime`; Bionic falls back to the real syscall. Optimisation: provide a real vdso `clock_gettime` that calls host `clock_gettime` via trampoline (yougram voip calls it at audio rates).
* `/proc/self/maps` shows both host and guest mappings (identity): ok; filter out the engine's JIT/TCG buffer if an app scans (anti-tamper libs).

## 6. Reuse map (Berberis: `frameworks/libs/binary_translation` = BT, `frameworks/libs/native_bridge_support` = NBS)

**Reuse as is (arch-neutral)**: `BT/base/` (logging, tracing, mmap helpers, raw_syscall), `BT/tiny_loader/`, `BT/proxy_loader/`, `BT/guest_loader/{guest_loader.cc, linker_callbacks.cc, linker_debug.cc, app_process.cc}` (just arch constants), `BT/jni/` (+`api.json`, generator), `BT/native_activity/`, `BT/native_bridge/native_bridge.cc` (control flow), `BT/guest_abi/` common templates, `BT/runtime_primitives/{host_function_wrapper_impl, known_guest_function_wrapper, guest_function_wrapper_impl}` (the *abstraction*; their implementation touches the translation cache - adapt to the engine trampoline table), `BT/kernel_api/*.cc` common emulations (`open`, `fcntl`, `exec`, `sys_mman`, `prctl`, `ptrace`, `unistd`, `sigevent`), `BT/guest_os_primitives/{guest_thread*, guest_map_shadow, guest_thread_map, guest_thread_key, guest_thread_manager, guest_signal_handling (generic part)}`, all of `NBS/android_api/*` incl. `api_arm64.json`, `stubs_arm64.*`, `vdso_arm64.S`, `init_static_tls_arm64.S`, `trampolines_arm64_to_x86_64-inl.h`, `NBS/guest_state/arm64`, `BT/guest_abi/arm64`, `BT/calling_conventions` (arm64 AAPCS64 + x86_64 SysV), `BT/prebuilt/system/etc/{init/berberis.rc, binfmt_misc/*, ld.config.riscv64.txt}` as templates, `BT/program_runner/` (host/binfmt runner; basis for our M1/M2 test tool), `BT/tests/hello_world`, `BT/tests/ndk_program_tests` (guest test programs - built with the native_bridge arch).
**Port (riscv64-specific, arm64 equivalent must be written)**: `BT/native_bridge/riscv64/native_bridge.cc` (3 constants), `BT/guest_loader/riscv64/{guest_loader_arch.cc (InitKernelArgs/auxv, paths), linker_callbacks_arch.cc}`, `BT/guest_os_primitives/riscv64/{guest_context_arch.h, guest_setjmp.cc (arm64 jmp_buf layout), guest_signal.cc, gen_syscall_numbers_*.cc}`, `BT/guest_os_primitives/guest_thread_pthread_create_arch.cc`, `guest_signal_handling_arch.cc`, `guest_signal_action_arch.cc`, `BT/kernel_api/riscv64/{syscall_emulation*.cc, guest_types.h, gen_syscall_emulation_riscv64_to_x86_64-inl.h}`, `BT/runtime/riscv64/` (init_guest_arch, run_guest_call: becomes engine glue), `BT/guest_state/riscv64`.
**Drop (replaced by the engine)**: `BT/{decoder, interpreter, lite_translator, heavy_optimizer, backend, assembler, intrinsics, code_gen_lib, exec_region, instrument, runtime/translator*, runtime_primitives/{translation_cache, code_pool, ...}, device_arch_info, tools/riscv*}`.
**Licensing**: Berberis is Apache-2.0. Apache-2.0 code can be combined into a GPLv3 (not GPLv2-only) work. Keep the GPL-2.0 engine (Unicorn) in its own `.so` loaded through `engine.h`'s plain C API; the bridge glue stays Apache-2.0 / GPL-2.0-or-later headers on new files as requested, but a *distributed* combined `libours_nb.so` that statically contains Apache code and links the GPLv2 engine must be treated as GPLv3 or kept split. Decision needed before first release.

## 7. Milestones

Test rig: QEMU `/mnt/storage-adata/gsi16/cf/run-pc-qemu.sh` (UEFI PC image `android-pc.img`; add `-drive file=/mnt/storage-adata/tmp/apkdisk/apps.img,format=raw,if=none,id=apps -device virtio-blk-pci,drive=apps,serial=apps`, mount in the guest `mount -o ro /dev/block/vdb /mnt/apps` or `vda/vdb` per probe; install with `pm install /mnt/apps/yougram.apk`; logcat via `hvc2` -> `$C/logcat.log`; shell via `$C/console.sock` + `con.py`). Faster dev loop: `adb`-less push by writing `/system` files into the image with `debugfs`/loop mount, or deliver bridge+guest libs on `apps.img` and `mount --bind` over `/system/lib64` (userdebug, `su 0`, `setprop` needs `ro.*` - so ro props require an image rebuild once; afterwards iterate by replacing `/system/lib64/libours_nb.so` through remount).
Effort = 1 engineer who knows the code base, includes debugging; excludes engine perf work.

| M | Goal | Work | Test / acceptance | Est. |
|---|---|---|---|---|
| **M0** | Empty bridge loads; `isSupported`/`isPathSupported` etc. answer; no guest code. | `libours_nb.so` from `translator/bridge/` (Soong), product props, `PRODUCT_PACKAGES`. All callbacks log; `loadLibrary` returns nullptr w/ error "not implemented". Possibly first trial of `m libc.native_bridge linker.native_bridge` (build check only). | Boot; `getprop ro.dalvik.vm.native.bridge`; logcat `OursNB: initialize(isa=arm64 ...)` when launching an arm64-only test APK (yougram.apk ships only `lib/arm64-v8a` + `lib/armeabi-v7a`, no x86 dirs, verified, so primary ABI must resolve to arm64-v8a); app starts and Java UI works; failure to load native lib is an `UnsatisfiedLinkError` with our message (proves routing). `dumpsys package <pkg> | grep -i abi` shows `primaryCpuAbi=arm64-v8a`. | 3-5 d (incl. image rebuild cycles) |
| **M1** | Run a static arm64 hello-world in-process, no Bionic. | Engine adapter (`engine.h` backed by Unicorn), TinyLoader mapping, minimal `SVC` handler with `write/exit_group/brk/mmap/...`, aarch64 stack/auxv set-up, `ours_nb_program_runner` host tool (modeled on `BT/program_runner`) that runs an ELF with no Android runtime; same code path later used from the bridge via a debug property (`debug.ours_nb.selftest=/path`). Guest binary: `BT/tests/hello_world` built static with the native_bridge arch, or a hand-assembled one. | `ours_nb_program_runner hello_static` prints `Hello!` on the x86_64 Android image (QEMU) and on the host (glibc build of runner for fast iteration - Unicorn already runs there, per 01 PoC). Syscall trace shows translated numbers. | 1-1.5 wk |
| **M2** | Dynamic arm64 Bionic executable: real `linker64` + `libc.so`/`libm`/`libdl` + proxy libc. | Build guest Bionic (`*.native_bridge`), `ld.config.arm64.txt`, vdso, `InitKernelArgs` for arm64, trampoline table (4.3), `proxy_loader` + libc proxy (`malloc`, `pthread_*`, props), `GuestThread`+TLS+static TLS init, `ProtectMappingsFromGuest`, syscall generator run for arm64, `fstat/epoll/clone/rt_sigaction` hand parts, `strtold/long double` conversions. Debug via guest `strace`-style TRACE. | Dynamic `hello` (printf, malloc, pthread_create + join, std::thread, C++ exception, `getenv`, file I/O, `dlopen` of a second arm64 .so, `qsort` callback, SIGSEGV handled by a guest handler + `sigaltstack`, `siglongjmp`) as `BT/tests/ndk_program_tests` style binaries; all must produce the same output as on real arm64 (or `qemu-aarch64` on the host, 01 says QEMU 10 is installed - use as oracle). | 3-5 wk |
| **M3** | JNI: `JNI_OnLoad` of a trivial JNI .so inside a real app. | Full NativeBridge callbacks (`initialize` starts guest app_process thread, `loadLibrary(Ext)`, namespaces w/ host fallback, `getTrampoline*`, `JNI_OnLoad` wrapper, JNIEnv/JavaVM tables from `api.json`, `RegisterNatives`, `getMethodShorty`, critical natives, `isNativeBridgeFunctionPointer`), `liblog` (guest or proxy), foreign-thread guest context, `FinalizeInit`. Test APK: tiny NDK app, arm64-v8a only, `System.loadLibrary` + native `stringFromJNI()` + `RegisterNatives` + callback to Java (`CallVoidMethod`, `NewStringUTF`, exceptions) + native thread `AttachCurrentThread`. Candidate from the yougram package: `libyg_example_hello.so` (5.8 KB, 7 imports) as the first real one; then `libyuv.so`, `libsurface_util_jni.so`, `libandroidx.graphics.path.so`. | Delivered through `apps.img`; `pm install`, launch, UI shows string from guest code; logcat shows guest `__android_log_print`; second process launch works (code cache); app killed cleanly; no ART abort. Crash test: guest null deref -> tombstone/ANR-free. | 2-3 wk |
| **M4** | yougram libs: `libtdjsonjava.so` (21 MB) then `libyougram_voip.so` (50 MB). | Perf work on engine glue, libz via guest build, OpenSL ES/EGL/GLES/libandroid/jnigraphics proxies enabled, `SIGSEGV/SIGBUS` robustness, `sched_getaffinity`/`/proc/cpuinfo`/hwcap emulation, 16 KiB-aligned LOAD mapping, `extractNativeLibs=false` path (`base.apk!/lib/arm64-v8a/`), `dl_iterate_phdr`, memory footprint (TCG buffers 50 MB lib!), startup time (TDLib init in Unicorn will be slow: **budget for it**; consider persisting a translation cache - needs engine support), atomics across threads. Steps: (a) tdjson: `td_json_client_create`/`td_send`/`td_receive` through Java `Client` JNI -> login screen reaches network; (b) voip: `System.loadLibrary` + `JNI_OnLoad` only (no call) then a call. | App cold start to first TDLib response < 15 s in QEMU w/ KVM (target, not guarantee); no crash for 10 min; logcat free of `SIGSYS`. Voip realtime audio is likely unachievable on Unicorn - define M4 acceptance as "loads, initialises, registers natives, call setup does not crash"; real-time quality is an engine-performance question (dynarmic/QEMU linux-user escape hatches). | 4-8 wk (high variance) |

Total to M3 ~7-10 wk; M4 depends on engine speed.

## 8. Open risks / things to verify early
1. `m libc.native_bridge linker.native_bridge libnative_bridge_vdso.native_bridge` on our tree for arm64 on x86_64 (Soong `native_bridge` target arch with `TARGET_NATIVE_BRIDGE_ARCH=arm64`; AOSP's `generic_x86_64_arm64` board exists as the reference config). Expect a few broken modules (bionic changes for 16 KiB page alignment).
2. Interceptable-stub patching vs Unicorn (4.3): confirm `DEFINE_INTERCEPTABLE_STUB_FUNCTION` expansion in `native_bridge_support/vdso/interceptable_functions.h` leaves room for a `b` (else use hook-by-address).
3. ART signal chain cooperation (5.4).
4. Memory footprint: 50 MB lib + TCG buffers + 4 engines (`uc_ctl_set_tcg_buffer_size`).
5. `ro.dalvik.vm.isa.arm64` side effects: app compile filter, `dex2oat` on arm64 isa dir, `LoadedApk.java:229` secondary-ISA logic (only for the 32-bit arm secondary ABI, which we leave unset).
6. GPL/Apache split (6).
7. Zygote preload/app-zygote: bridge `initialize` runs after fork for each app (guest process start = linker + libc init under Unicorn per app launch, est. 100-400 ms cold; Berberis has the same cost). Defer via lazy start (`FinalizeInit` is already lazy at first `loadLibrary`; move `StartAppProcessInNewThread` there for non-bridged apps? Not needed: bridge is only initialised for apps whose ISA is arm64).
