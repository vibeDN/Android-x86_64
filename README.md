# Android-x86_64

Android (AOSP 16) for PCs, **work in progress**. Goal: a modern, open, good-looking desktop Android
for x86_64 machines (DeX-style windowing + Material You), with root platform support
(KernelSU/Zygisk) and an ARM→x86 translation layer planned.

## Status

- AOSP 16 (`android16-qpr2-release`) builds for x86_64 from our own product `ours_cf_x86_64_phone`, on our own
  kernel (Android common kernel 6.12 + Rust) with our own modules.
- **Boots on real hardware** (tested: ASUS TUF Gaming F17, i5-12500H) from a USB stick: UEFI + GRUB (Material You
  theme), native resolution through the UEFI framebuffer (simpledrm), Ethernet, keyboard/mouse, adb over the network.
- Desktop windowing from the image (apps open in freeform windows), Lawnchair 16-dev as the home screen/taskbar.
- **armtr**, our own ARM64 -> x86_64 translator (native bridge, GPL-2.0, Unicorn/QEMU TCG based): arm64-only apps run,
  e.g. Telegram clients (TDLib), VLC, Organic Maps, Kodi, Mindustry, Syncthing; arm64 executables via binfmt_misc.
  See `docs/translator/`.
- Simple disk installer (whole-disk, v1): `scripts/installer/`, `docs/INSTALL.md`.
- **Not done yet:** GPU acceleration (Mesa iris/radeonsi, NVK for NVIDIA), Wi-Fi, audio, KernelSU/Zygisk,
  DeX-style taskbar per `design/`, Firefox under the translator.

## Layout

| Path | What |
|------|------|
| `device/ours/pc_x86_64/` | Our product definition: desktop-mode properties, PC feature, freeform feature, display defaults, framework RRO overlay |
| `patches/` | Small patches against AOSP (cuttlefish, frameworks, minigbm, kernel, Lawnchair) |
| `translator/` | armtr ARM64 -> x86_64 translator (GPL-2.0-or-later) |
| `kernel/` | Kernel config used for our build (derived from the Cuttlefish GKI config) |
| `scripts/build/` | Build helpers: product build, kernel+modules build, module directory staging, memory guard |
| `scripts/qemu/` | QEMU boot helpers: disk assembly, run script, QMP input/screenshot helpers |
| `design/mockups/` | HTML mockups of the planned desktop shell (taskbar, app menu, quick settings) |
| `docs/` | Engineering notes |

## How it fits together

1. Sync AOSP `android16-qpr2-release` (`repo`), put `device/ours/pc_x86_64/` into `device/ours/`.
2. Apply `patches/` to `device/google/cuttlefish`.
3. Build the kernel and modules (`scripts/build/build-kernel-cf.sh`), stage modules
   (`scripts/build/mkmoddirs.py`), build images (`scripts/build/build-aosp-own.sh`).
4. Assemble a GPT disk and boot in QEMU (`scripts/qemu/`). See `docs/NOTES.md` for the kernel
   command line, devices and pitfalls.

Paths in the scripts (`/mnt/storage-adata/...`) are the author's layout; adjust them.

## Licensing

Our own files in this repository are Apache-2.0 (see `LICENSE`). Nothing from AOSP, the Linux
kernel tree, Google GSI images or prebuilt binaries is redistributed here: AOSP is Apache-2.0,
the Linux kernel is GPL-2.0, Google Sans fonts are loaded from Google Fonts (SIL OFL) by the
mockups only. The kernel config is a configuration file derived from the Android GKI config
(GPL-2.0 project). Proprietary firmware and Google Mobile Services are not part of this project.

## Disclaimer

Experimental. Not affiliated with Google. Play Integrity (Device/Strong) is out of scope.
