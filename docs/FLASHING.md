# Running on real hardware (first bring-up)

Experimental. Use a **separate USB stick or spare disk** (32 GB or more). Never write the image to a disk
you care about, and double-check the device name: `dd` overwrites without asking.

## Before you start

- UEFI boot only. **Disable Secure Boot** (our GRUB and kernel are not signed).
- Expect the first boot to use the UEFI framebuffer (`simpledrm`) with software rendering (SwiftShader):
  it works on any PC, but it is slow. Native GPU drivers (Intel/AMD, Mesa NVK for NVIDIA) come later.
- Wi-Fi and most laptop-specific hardware are not included yet. USB keyboard/mouse, SATA, NVMe and
  USB storage are.

## Build the image

```
scripts/qemu/make-pc-image.sh android-pc.img 18
```

(see `docs/NOTES.md` for the environment variables; the image is sparse, 2 GB on disk, 18 GB logical.)

## Write it

1. Plug the stick in and find its device: `lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS`.
   It must be the stick (`TRAN=usb`), unmounted, and not your system disk.
2. `sudo dd if=android-pc.img of=/dev/sdX bs=4M conv=fsync status=progress` (replace `sdX`).
3. Boot from the stick (firmware boot menu, usually F8/F11/F12). The GRUB menu shows
   "Android (x86_64)"; it starts by itself after 3 seconds.

## If it does not boot

- GRUB: press `e` on the menu entry to edit the kernel parameters; `c` opens a GRUB console.
- Add `console=ttyS0 androidboot.console=ttyS0` (serial) or `drm.debug=0x14` to see more.
- The disk is found by the GUID of the `boot_a` partition (`androidboot.boot_part_uuid`), so the stick
  can be on any USB port or controller.
