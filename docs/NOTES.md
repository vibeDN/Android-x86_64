# Engineering notes

Findings from bringing AOSP 16 up on x86_64 without the `cuttlefish-common` host packages.

## Booting the images in QEMU

- QEMU + KVM, kernel from `boot.img`, initrd = vendor ramdisk + `init_boot` ramdisk (concatenated).
- GPT disk with: `misc`, `vbmeta_a`, `vbmeta_system_a`, `vbmeta_vendor_dlkm_a`, `vbmeta_system_dlkm_a`,
  `metadata`, `super`, `userdata`, `boot_a`, `init_boot_a`, `vendor_boot_a`, **`frp`** (required:
  without it `PersistentDataBlockService` kills `system_server`).
- Kernel command line (see `scripts/qemu/run-qemu.sh` and the `*-args.txt` files):
  `androidboot.force_normal_boot=1` (otherwise it boots recovery), `androidboot.hardware=cutf_cvm`,
  `androidboot.fstab_suffix=cf.ext4.cts`, `androidboot.slot_suffix=_a`,
  `androidboot.boot_devices=pci0000:00/0000:00:12.0`, graphics (`gralloc=minigbm`,
  `hwcomposer=drm`, `egl=angle`, `vulkan=pastel`, `lcd_density=160`), vsock ports for the lights /
  camera / tombstone HALs, `androidboot.vendor.apex.*` to pick one APEX of each duplicated HAL pair.
- Devices: virtio-blk (slot 0x12), virtio-gpu, virtio-keyboard, **virtio-mouse** (relative; the
  virtio tablet sends `BTN_MOUSE`, which Android ignores for touch devices), virtio-rng, vhost-vsock,
  virtio-serial with 11 consoles (`hvc2` = logcat, `hvc10` = oemlock HAL port).
- Serial console to a unix socket gives a `shell` prompt (`su 0` works on userdebug). QMP gives
  input injection and `screendump` (the hardware cursor is not part of the screendump).

## Kernel

- ACK `android16-6.12`. `ashmem` exists only as a Rust driver in 6.12, so `CONFIG_RUST=y` is needed.
  Build with the AOSP prebuilts (`prebuilts/rust/linux-x86/<ver>`, `prebuilts/clang-tools/.../bindgen`)
  and pass `RUSTC`, `HOSTRUSTC`, `BINDGEN`, `RUST_LIB_SRC` as make arguments.
- Modules in `vendor_dlkm` / `system_dlkm` must match the kernel exactly (vermagic + symbol CRCs),
  otherwise `dlkm_loader` aborts and `mediaswcodec` crashes (no dma-buf `system_heap`).
  `BoardConfig.mk` takes `SYSTEM_DLKM_SRC` and `KERNEL_MODULES_PATH` with `?=`, so point them at
  directories of your own modules and rebuild only the images.

## Product

- `device/ours/pc_x86_64/ours_cf_x86_64_phone.mk` inherits the Cuttlefish phone product.
- Device properties must go to `PRODUCT_PRODUCT_PROPERTIES` / vendor (not `system`: the GSI check fails).
- Desktop windowing needs: `persist.wm.debug.desktop_experience_devopts=true`,
  `persist.wm.debug.desktop_mode_enforce_device_restrictions=false`, the framework RRO
  (`config_isDesktopModeSupported`, `config_canInternalDisplayHostDesktops`,
  `config_isDesktopModeDevOptionSupported`), `android.software.freeform_window_management`,
  `android.hardware.type.pc`, a display of at least 600dp smallest width (160 dpi at 1280x800), and a
  freeform default for the display (`display_settings.xml` entry with `windowingMode="5"`).
- `wm shell desktopmode moveTaskToDesk` crashes SystemUI on this setup; launch with
  `am start --windowingMode 5` or set the display windowing mode instead.
- Cuttlefish-only HALs can be dropped with `CF_VENDOR_NO_UWB=true`, `CF_VENDOR_NO_THREADNETWORK=true`,
  `LOCAL_ENABLE_OEMLOCK=false`.

## Build machine notes

- 15 GB RAM is tight: use `-j4`, a large swap, and `scripts/build/memguard.sh` (pauses `ninja` when
  `MemAvailable` drops below ~800 MB). R8 jobs and the Soong analysis step peak at 10+ GB.
- `dd conv=sparse` over an existing disk leaves stale blocks: punch holes first (`fallocate -p`).

## Desktop-first on a single internal display (Android 16)

Findings from reading WMShell (`DesktopDisplayEventHandler`, `DesktopDisplayModeController`):

- With the multiple-desks backend, the default desk of the internal display is created only while the
  keyguard is not locked ("handled on unlock" only covers non-default displays). A desktop product
  should not show a lock screen unless a credential is set: `config_disableLockscreenByDefault=true`.
- `getTargetWindowingModeForDefaultDisplay()` switches the display to touch-first (fullscreen) unless a
  physical keyboard **and a touchpad** are connected (flag `form_factor_based_desktop_first_switch`).
  A plain USB mouse is not a touchpad, so for a PC product the flag is disabled
  (`patches/0003-release-disable-form-factor-desktop-first.patch`) and the display keeps the freeform mode
  from `display_settings.xml`.
- The dev-option override (`persist.wm.debug.desktop_experience_devopts=true`) forces every flag marked
  overridable in `DesktopExperienceFlags` to true, so the release-config value alone was ignored; the
  form-factor flag is made non-overridable in the enum
  (`patches/0004-frameworks-form-factor-flag-not-overridable.patch`). With both patches a clean system
  opens every app in a freeform window of the default desk.
- `ignoreOrientationRequest="true"` (taken from the Cuttlefish foldable product) lets windows keep their
  shape when an app asks for a fixed orientation.
- The Cuttlefish foldable product itself only adds hinge/lid device states and a two-display layout
  (emulated by host-side daemons); it does not enable desktop windowing.

## Booting on a PC (UEFI)

- `scripts/qemu/make-pc-image.sh` builds one GPT disk image: EFI system partition (standalone GRUB,
  kernel, initrd, theme) plus all Android partitions. `androidboot.boot_part_uuid=<GUID of boot_a>` lets
  Android find its boot disk on any controller (USB / SATA / NVMe), no PCI path needed.
- Verified in QEMU with OVMF and the disk attached as NVMe (`scripts/qemu/run-pc-qemu.sh`).
- The standalone GRUB starts on its memdisk: `search --label --set=root EFI` is required, and the modules
  (`part_gpt fat search_label gfxterm gfxmenu png ...`) must be listed for `grub-mkstandalone`.
- GRUB theme (`boot/grub-theme/`, Material You, generated by `gen-theme.sh`): theme properties are one per
  line (no `;`), slices must be 8-bit RGBA PNG, fonts must have glyphs for the text (Noto Sans: no arrows),
  and the terminal font has to be set explicitly (`gfxterm_font`), otherwise the biggest loaded font wins.
- On real PCs without a native GPU driver the display is `simpledrm` (UEFI GOP framebuffer, display id
  `local:0`): `display_settings.xml` has an entry for it. QEMU needs a plain `VGA` device to show that
  framebuffer (virtio-gpu does not).

## UEFI framebuffer path (simpledrm) on Android

Needed for the first boot on any PC without a native GPU driver. Three fixes were required:

1. `simpledrm` only exposes the native framebuffer format, while `drm_hwcomposer` asks for formats with
   alpha (`ARGB8888`, `ABGR8888`, `XBGR8888`): `patches/kernel/0001-simpledrm-accept-argb8888.patch`
   accepts them and ignores / swaps channels as needed.
2. `minigbm` had no backend for `simpledrm` and picked `vkms` (card1) for allocation; buffers allocated on
   another device cannot be imported by the hwcomposer: `patches/aosp/0005-minigbm-simpledrm-dumb-backend.patch`
   registers a dumb backend and drops DRM master after opening a card node (the first opener becomes master,
   and the hwcomposer needs it).
3. Test display: legacy VGA (`-device VGA`) switches to an 8-bit mode after the kernel starts and shows garbage.
   Use `ramfb` (plain linear framebuffer, like a UEFI GOP on real hardware) in QEMU.
