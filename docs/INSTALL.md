# Installing Android-x86_64 to a Disk

## Quick Start (Live USB Mode)

1. Boot Android from the live USB / installation media.
2. Connect via adb: `adb connect <device-ip>:5555`
3. Run the installer: `adb shell su 0 /system/bin/install.sh`
4. Select target disk, type `ERASE` to confirm.
5. Reboot into the target disk.

## Test Mode (Host Development)

Test against image files without touching real disks:

```bash
# Create blank target image
truncate -s 16G /tmp/target.img

# Run installer in test mode
./scripts/installer/install.sh \
  --source-image /path/to/source.img \
  --target-image /tmp/target.img \
  [--dry-run]

# Boot result in QEMU
./tmp/run-pc-adb.sh /tmp/target.img
```

## Safety Notes

- **Never use real block devices during development** — always test with image files first.
- The installer requires typing the word `ERASE` to proceed; this is intentional.
- Source disk is auto-detected from `/proc/cmdline androidboot.boot_part_uuid`; confirm the list before selecting target.

## Partition Layout

The installer recreates the GPT layout from `make-pc-image.sh`:

- **EFI** (ESP): 512 MiB, FAT32, contains GRUB, kernel, initrd, theme
- **misc, vbmeta_*, metadata, super, boot_a, init_boot_a, vendor_boot_a, frp**: Fixed sizes (see code)
- **userdata**: Rest of disk (formatted by Android on first boot)

## Implementation Details

### Boot GUID Handling

The kernel finds its disk via the `boot_a` partition GUID in the GRUB kernel command line (`androidboot.boot_part_uuid=<GUID>`). The current v1 implementation:

- **Approach**: Keep the same GUID as the source disk (see `randomize_boot_guid()`).
- **Why**: The GRUB bootloader command line is embedded in `BOOTX64.EFI` as a memdisk, so updating the ESP's `grub.cfg` copy is not sufficient; the binary would need to be rebuilt.
- **Consequence**: Both source and target disks briefly have the same GUID after installation, but this is safe because they won't run simultaneously. Android uses the UUID to identify its boot partition on first boot and never changes it.

**v2 item**: Regenerate `BOOTX64.EFI` with fresh GUID, or extract/repack the memdisk.

## Known Limitations

- **Live USB installer environment not yet built**: The script currently runs as root from the Android shell (`adb shell su 0`). A dedicated installer environment (v2 item) would remove this dependency.
- **GRUB menu integration**: No "Install to disk" menu entry in the boot GRUB; must be run manually from Android shell.
- **Alongside existing OS**: v1 does whole-disk mode only. Multi-boot into free space is a v2 item.
- **No undo**: Installation is immediate and irreversible.

## v2 Roadmap

- [ ] Dedicated installer environment (initramfs + busybox, boots from GRUB menu entry)
- [ ] Fresh UUID generation with `BOOTX64.EFI` rebuild
- [ ] Install alongside other OS into free space
- [ ] `efibootmgr` equivalent to register boot entry
- [ ] Pre-flight checks: disk space, firmware support, existing partitions
- [ ] Rollback mechanism
