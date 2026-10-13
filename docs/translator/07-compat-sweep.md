# 07 - Compatibility sweep: FLOSS apps with arm64-v8a-only native code

Date 2026-10-13. Image: copy of `/mnt/storage-adata/tmp/net-pc.img` (`/mnt/storage-adata/tmp/compat-pc.img`), QEMU/KVM
800x600 ramfb, Ethernet (slirp). Bridge as described in doc 05. Translator not modified.

## Method

* APKs: F-Droid `index-v2.json` (fetched 2026-10-13); for each package the newest version whose `nativecode` has
  `arm64-v8a` and no `x86_64`, otherwise the newest universal APK. Downloaded to `/mnt/storage-adata/apps/compat/`, sha256
  checked against the index (`SHA256SUMS` there, all OK). ABIs/launcher checked with `aapt2 dump badging`. Universal APKs
  (x86_64 present) were installed with `pm install --abi arm64-v8a` to force translation (`primaryCpuAbi=arm64-v8a`
  checked with `pm dump` for every app).
* APK disk: `mke2fs -t ext4 -d /mnt/storage-adata/apps/compat /mnt/storage-adata/tmp/compat-apkdisk/apps.img 2200M`, runner =
  `armtr-run-pc.sh` with only the apps.img path changed. Guest: `pm install -g`, launch via `cmd package resolve-activity`
  + `am start -W`, wait 40-90 s, screenshot, then a few `input tap`s to reach the native part (no accounts, no text
  entered anywhere). Media tests: a 20 s H.264/AAC MP4 served from the host
  (`http://10.0.2.2:8000/test.mp4`).
* Evidence: per-app logcat slices in `/mnt/storage-adata/apps/compat-logs/<pkg>[-N].log`, screenshots in
  `/mnt/storage-adata/apps/compat/shots/`. Static check: `readelf -d` DT_NEEDED of every `lib/arm64-v8a/*.so` against the
  APK's own libs + the guest dir `/system/lib64/armtr` (`compat-logs/needed-static.txt`).
* Guest library set on the image (`/system/lib64/armtr`): ld-android, libc, libm, libdl, libc++, liblog, libz, libandroid,
  libnativewindow, libjnigraphics, libEGL, libGLESv1_CM, libGLESv2, libGLESv3, libOpenSLES. Nothing else.

## Results

| App (package) | Version | ABI in APK | Native libs (main) | Result | First failure | Screenshots (`apps/compat/shots/`) |
|---|---|---|---|---|---|---|
| VLC (`org.videolan.vlc`) | 3.7.1 | arm64 only | libvlc, libvlcjni, libc++_shared | **works** (partial HW) | HTTP stream via guest sockets, avcodec SW decode of H.264+AAC, gles2 vout, audiotrack aout all start. `libvlc decoder: MediaCodec NDK init failed` (libmediandk.so missing, B1), falls back to SW | `org.videolan.vlc.png`, `org.videolan.vlc-play.png` |
| mpv-android (`is.xyz.mpv`) | 2026-09-17 | arm64+v7a+x86_64 (forced) | libmpv, libav* | **crashes** on play | UI (Java file picker) ok; opening a video: `UnsatisfiedLinkError: library "libmediandk.so" not found: needed by .../libavcodec.so` (B1) | `is.xyz.mpv.png`, `is.xyz.mpv-play.png` |
| Organic Maps (`app.organicmaps`) | 2026.09.29-32 | universal (forced) | liborganicmaps | **works** | World map (70 MB, downloaded by the app) renders with GLES through the proxies; no native error | `app.organicmaps-4.png` |
| Fennec F-Droid (`org.mozilla.fennec_fdroid`) | 157.0.0 | arm64 only | libxul, libmozglue, libnss3, libmegazord | **crashes** at start | `UnsatisfiedLinkError: Unable to load library 'xul': library "libmediandk.so" not found: needed by .../libxul.so` (B1); libxul also NEEDs `libsync.so` (B1) | `org.mozilla.fennec_fdroid.png` (StartupCrashActivity) |
| OsmAnd~ (`net.osmand.plus`) | 5.4.10 | arm64 only | libOsmAndCoreWithJNI, libQt5*, libosmand | **partial** | Legacy renderer (libosmand.so) draws the map; OpenGL core fails: `library "libstdc++.so" not found: needed by .../libOsmAndCoreWithJNI.so` (B2) -> no OpenGL map/weather | `net.osmand.plus.png`, `net.osmand.plus-2.png` |
| Element Classic (`im.vector.app`) | 1.6.62 | arm64 only | librealm-jni, libjnidispatch, libmaplibre (+ matrix_sdk_crypto_ffi not loaded) | **works** (to onboarding) | none; Rust crypto (libmatrix_sdk_crypto_ffi) is only loaded after login, not exercised | `im.vector.app.png` |
| Telegram X (`org.thunderdog.challegram`) | 0.28.11 | arm64 only | libtdjni (TDLib), libtgcallsjni, libtgxjni, libcryptox/libsslx | **works** (to phone entry) | none; TDLib reaches the phone screen, 2 established TCP connections, country auto-detected | `org.thunderdog.challegram-2.png` |
| InviZible Pro (`pan.alexander.tordnscrypt.stable`) | 7.5.0 | arm64 only | libtor, libdnscrypt-proxy, libi2pd ... (executables named lib*.so) | **partial** (UI only) | START TOR: `Error Tor: 1 ERR=.../lib/arm64/libtor.so[1]: syntax error: unexpected '('` - arm64 ELF exec gets ENOEXEC, sh then parses it as a script (B3) | `pan.alexander.tordnscrypt.stable-2.png` |
| Tor VPN beta (`org.torproject.vpn`) | 1.9.1Beta | arm64 only | libgojni (Go), libonionmasq_mobile (Rust/arti), libsqliteJni | **partial** | Go+Rust libs load, VPN tun0 comes up, arti connects to guards and fetches consensus + certs, then microdescriptor downloads fail repeatedly: `tor_dirclient: Retiring circuit because of directory failure: Partial response`, `Invalid document ... line truncated before newline` (B4). Never bootstraps (90 s+) | `org.torproject.vpn-3.png` |
| Syncthing-Fork (`com.github.catfriend1.syncthingfork`) | 2.1.6.0 | universal (forced) | libsyncthingnative (Go executable) | **partial** (UI only) | `SyncthingRunnable: exit reason = exitError` right after start, config.xml never written: the daemon is an executed arm64 ELF (B3) | `com.github.catfriend1.syncthingfork-3.png` |
| Mindustry (`io.anuke.mindustry`) | 8-fdroid-160.7 | universal (forced) | libarc, libarc-freetype | **crashes** | `FATAL EXCEPTION: GLThread ... library "libstdc++.so" not found: needed by .../libarc-freetype.so` (B2) | `io.anuke.mindustry.png` |
| SuperTuxKart (`org.supertuxkart.stk`) | 1.4 | universal (forced) | libmain, libSDL2 (GLES, OpenAL->OpenSL ES) | **works** | Main menu rendered (~60 s cold incl. asset extraction); only an app-level "resolution too low" dialog (800x600) | `org.supertuxkart.stk.png` |
| PPSSPP (`org.ppsspp.ppsspp`) | 1.19.3 | arm64+v7a+x86_64 (forced) | libppsspp_jni (+ hook libs) | **works** | UI, native HTTP homebrew store, 35 MB download+unzip, and the homebrew game "Ozone" boots and renders (default CPU core; which core ran was not verified) | `org.ppsspp.ppsspp-4.png`, `org.ppsspp.ppsspp-7.png` |
| Kodi (`org.xbmc.kodi`) | 21.3 | arm64 only | libkodi + addon libs | **crashes** | Java splash ok, then `UnsatisfiedLinkError: library "libmediandk.so" not found: needed by .../libkodi.so` (B1); addon `libgame.libretro.2048.so` also NEEDs `libstdc++.so` (B2) | `org.xbmc.kodi.png` (Java splash before the crash) |
| Tux Paint (`org.tuxpaint`) | 0.9.35 | arm64 only | libSDL2*, ~70 magic-tool libs | **works** | Canvas + drawing with `input swipe` ok. 206x `libEGL: eglMakeCurrentImpl:1100 error 3002 (EGL_BAD_ACCESS)` from one thread during the first 13 s (B6). Launcher entry resolves to a chooser (2 launcher activities), app-side | `org.tuxpaint.png`, `org.tuxpaint-2.png` |
| Godot Editor 4 (`org.godotengine.editor.v4`) | 4.7.2 | arm64 only | libgodot_android | **crashes** at start | `UnsatisfiedLinkError: library "libcamera2ndk.so" not found: needed by .../libgodot_android.so` (B1; it also NEEDs libmediandk.so) | `org.godotengine.editor.v4.png` |

Summary: 16 apps - 7 work (VLC, Organic Maps, Element, Telegram X, SuperTuxKart, PPSSPP, Tux Paint), 4 partial (OsmAnd, InviZible, Tor VPN, Syncthing), 5 crash (mpv, Fennec, Mindustry, Kodi, Godot).
No guest fault, SIGILL, armtr warning/error, "unavailable host function" message, ENOSYS log or native tombstone from any
tested app (the only tombstones on the image are `com.android.bluetooth` aborts, unrelated). Every crash is a missing
platform library or exec, not a translation error.

## Bug list (by impact)

**B1 - NDK platform libs libmediandk / libcamera2ndk / libsync are not provided to the guest** (owner: bridge guest libs,
`translator/bridge/guest/gen_stubs.py` + `translator/hostlibs`). The guest linker fails the DT_NEEDED and the whole app
library fails to load. Hard crash: Fennec (libxul: libmediandk + libsync), Kodi (libkodi), mpv (libavcodec), Godot
(libcamera2ndk + libmediandk). Soft: VLC loses MediaCodec ("MediaCodec NDK init failed", SW fallback). Evidence:
`compat-logs/{org.mozilla.fennec_fdroid,org.xbmc.kodi-3,is.xyz.mpv-play,org.godotengine.editor.v4,vlc-play}.log`,
static list `compat-logs/needed-static.txt`. Fix scope: add proxy libs (`AMediaCodec_*`, `AMediaFormat_*`,
`AMediaExtractor_*`, `AMediaCrypto_*`, `ACameraManager_*`, `sync_wait`, ...). Note that loading must succeed even if the
functions are never called; a stub-only lib (resolve-on-first-call, which gen_stubs already does) would unblock all four.

**B2 - libstdc++.so (the NDK system "minimal C++ runtime") missing in `/system/lib64/armtr`.** Old/CMake builds still list
it in DT_NEEDED. Crash: Mindustry (libarc-freetype). Degraded: OsmAnd (OpenGL core lib), Kodi addon libgame.libretro.2048.
Fix: ship the arm64 `libstdc++.so` from the guest sysroot (bionic builds it) in the guest lib dir. Evidence:
`compat-logs/{io.anuke.mindustry,net.osmand.plus}.log`.

**B3 - arm64 executables cannot be exec'd** (owner: runtime/armtr-run + system integration). Apps that ship tools as
`lib*.so` executables and run them via ProcessBuilder/sh get ENOEXEC; `/system/bin/sh` then tries the ELF as a script.
Device: `libtor.so --version` -> `not executable: 64-bit ELF file`; no `/system/bin/armtr/armtr-run`, no binfmt_misc
registration. Affected: InviZible Pro (tor, dnscrypt-proxy, i2pd, obfs4proxy, snowflake), Syncthing-Fork (Go daemon),
likewise Orbot/Termux-style apps. Needs an exec path (binfmt_misc `F` flag with armtr-run, or an execve hook in the
guest for guest-originated exec plus binfmt_misc for ART-originated ProcessBuilder).

**B4 - Tor VPN / arti: directory downloads end as "Partial response" / truncated documents** (owner: engine + runtime,
needs triage). Consensus and certificates download fine through the guest network stack, but the parallel microdescriptor
fetches (9108 missing) are cut short, so bootstrap never completes; the process sits at ~108% CPU. Two hypotheses:
(a) arti's per-request timeouts expire because the Unicorn engine is too slow for Rust crypto+zstd at this volume;
(b) a translation bug in a socket/epoll or read path that returns short data/EOF early. Repro: install
`org.torproject.vpn_242.apk`, `appops set org.torproject.vpn ACTIVATE_VPN allow`, tap Start testing, Connect; grep
`onionmasq` in logcat. Evidence: `compat-logs/org.torproject.vpn-3.log`. Same app on an arm64 device / with a higher
arti timeout would split (a) from (b).

**B5 - Missing optional NDK libs that apps dlopen at run time: libvulkan.so, libaaudio.so.** Not a crash in this sweep
(SDL2/STK/Tux Paint/mpv/PPSSPP fall back to GLES and OpenSL ES), but Vulkan back-ends (PPSSPP, Godot, Fennec's WebRender
Vulkan path) and low-latency audio are unavailable. Found by string scan of the libs: libvulkan in liborganicmaps,
libgodot_android, libSDL2 (STK, Tux Paint), libppsspp_jni, libxul; libaaudio in libmpv, libSDL2, libxul.

**B6 (minor) - EGL_BAD_ACCESS burst in Tux Paint (SDL2).** 206x `eglMakeCurrentImpl:1100 error 3002` from one thread
(tid 21974) during startup, then rendering is fine. SuperTuxKart (same SDL2) shows none. Might be SDL behaviour or a
guest-thread/host-thread mismatch in the EGL proxy (context current on the thread that ran the guest's
`eglMakeCurrent` vs. the host thread). Evidence: `compat-logs/org.tuxpaint.log`.

**B7 (cosmetic) - diagnostics.** (1) dlopen errors carry a doubled prefix: `dlopen failed: dlopen failed: library ...`.
(2) `armtr loadLibraryExt(...)` log lines get cut by logcat's line limit before the result for long app paths
(Telegram X), so the `-> 0x...`/error is lost. (3) Guest `/proc/self/exe` resolves to `/system/bin/armtr/armtr_guest`
(OpenAL: `Got binary: "/system/bin/armtr", "armtr_guest"`); harmless here, but apps that locate config/data next to
their executable will look in `/system/bin/armtr/`.

## Re-test (AOSP15 image with new guest libraries) — 2026-10-13

**Setup:** Fresh AOSP15 build at `/mnt/storage-adata/aosp15/out/target/product/vsoc_x86_64/` includes all missing NDK platform libs in `/system/lib64/armtr/`:
- libmediandk.so (79 KB) — fixes B1 for Kodi, mpv, Godot, Fennec
- libcamera2ndk.so (20 KB) — fixes B1 for Godot
- libsync.so (3 KB) — fixes B1 (libxul dep, Fennec)
- libaaudio.so (20 KB) — B5 fix candidate
- libvulkan.so (147 KB) — B5 fix candidate
- libstdc++.so (52 KB) — fixes B2 for Mindustry, OsmAnd, Kodi addon

**Limitation:** The disk image created from the new build (`/mnt/storage-adata/tmp/retest-pc.img` via `make-pc-image.sh`) did not boot in QEMU (stuck in UEFI PXE/HTTP boot; the issue was not triage'd, likely bootloader/kernel config). Testing proceeded on the previous working image (compat-pc.img, which is based on an earlier AOSP build without these libs) with selected apps to establish that:

1. **B1 (libmediandk / libcamera2ndk / libsync) remains the root cause** of crashes in Fennec, Kodi, mpv, Godot. Logcat shows identical `dlopen failed: library "libmediandk.so" not found` error in the old image, confirming these libs were not present in the previous build either.

2. **B2 (libstdc++.so) status unknown** — could not test OsmAnd/Mindustry on the new image.

3. **B3 (arm64 exec) status unknown** — binfmt_misc empty in old image, new image boot failure prevented verification.

4. **B4 / B5 / B6 / B7 — untested** on new image.

**Recommendation:** Boot the AOSP15 image directly (or build a bootable variant) to confirm B1/B2 fixes and measure any new regressions. The new libraries are present in the build; tests on that image would be authoritative.

| App (package) | Tested on old image | Status | Notes |
|---|---|---|---|
| Fennec (org.mozilla.fennec_fdroid) | Yes | B1 crash (libmediandk) | Same as before; would expect FIXED on AOSP15 image |
| mpv (is.xyz.mpv) | Yes | Launches (not tested native) | x86_64 libs loaded (--abi not applied); B1 crash expected on arm64 path |
| VLC (org.videolan.vlc) | Yes | Perm error, untested | Would expect same partial HW behavior if native libs load |
| Fennec, Kodi, mpv, Godot, Mindustry | — | — | **Need retest on AOSP15 image when bootable** |

## Not covered

Login-gated native paths (Element's matrix-rust-sdk crypto, Telegram calls/tgcalls), PPSSPP CPU-core choice/JIT
verification, audio output quality/latency, performance numbers beyond the cold-start notes above. SchildiChat, Molly,
ScummVM, Orbot, Kiwix are not in the F-Droid main index under the expected IDs (or ship x86_64); Tor VPN and InviZible
cover the Tor case.

## Re-test via adb on the hardware image (android-pc-hw14.img, QEMU, density 160)

Checked by hand over `adb connect` (adbd on tcp:5555, host key preinstalled):
- binfmt_misc registered (`arm64_exe`, `arm64_dyn`); an arm64 Bionic binary runs straight from `adb shell` (B3 core fixed).
- Mindustry, Kodi, Syncthing-Fork: start and show their UI (previously crashed / UI-only).
- Fennec: still crashes. Two separate failures (new bug B8):
  - main process: `mozilla.appservices.fxaclient.InternalException: Callback interface method returned unexpected error` (Rust uniffi callback through JNA; suspect JNA/libffi closures under the translator);
  - Gecko child process: `Fatal signal 11 (SIGSEGV), SEGV_MAPERR, fault addr 0x0` in thread `Gecko` shortly after `GeckoLoader.nativeRun`.
- Note: QEMU boot needs `bootindex=0` on the NVMe disk (shared OVMF_VARS otherwise prefers PXE once a NIC is present).
- B8 follow-up: the bridge now invalidates translations when guest memory becomes executable (set_exec -> armtr_mem_protect).
  Re-test on the rebuilt image: Fennec STILL fails with the same uniffi `InternalException` in the main process, so the
  libffi-closure/SMC hypothesis did not explain it. Next: trace the JNA callback path (libjnidispatch `callback_dispatch`,
  the Java CallbackReference return value and the uniffi RustCallStatus struct passed by pointer) under armtr. Parked.
