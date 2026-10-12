#!/bin/bash
# Build a UEFI-bootable disk image (GPT) with an EFI system partition (GRUB + kernel + initrd) and all
# Android partitions. Usable in QEMU+OVMF and writable to a USB stick / spare disk.
#
# usage: make-pc-image.sh <out.img> [size_GiB]
# needs: sgdisk, mkfs.vfat, mtools (mcopy/mmd), grub-mkstandalone, simg2img (from the AOSP host tools)
set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"

OUT=${1:?usage: $0 out.img [size_GiB]}
SIZE=${2:-18}
P=${PRODUCT_OUT:-/mnt/storage-adata/aosp15/out/target/product/vsoc_x86_64}
H=${HOST_BIN:-/mnt/storage-adata/aosp15/out/host/linux-x86/bin}
C=${CF_DIR:-/mnt/storage-adata/gsi16/cf}          # unpacked boot images: $C/boot/kernel, $C/initrd.img
CMDLINE_EXTRA=${CMDLINE_EXTRA:-}
WORKDIR=${WORKDIR:-/mnt/storage-adata/tmp}; mkdir -p "$WORKDIR"
W=$(mktemp -d -p "$WORKDIR")
trap 'rm -rf "$W"' EXIT

# Partition layout, sectors of 512 B. ESP first, Android partitions as in the QEMU test disk.
ESP_START=2048;          ESP_END=$((2048 + 1024*1024 - 1))                 # 512 MiB
off=$((ESP_END + 1))
part() { # name size_sectors -> sets START/END
  START=$off; END=$((off + $2 - 1)); off=$((END + 1)); }
rm -f "$OUT"; truncate -s ${SIZE}G "$OUT"
sgdisk -o "$OUT" >/dev/null
sgdisk -n 1:$ESP_START:$ESP_END -c 1:EFI -t 1:EF00 "$OUT" >/dev/null
i=2
declare -A START_OF IDX_OF
for spec in misc:2048 vbmeta_a:2048 vbmeta_system_a:2048 vbmeta_vendor_dlkm_a:2048 vbmeta_system_dlkm_a:2048 \
            metadata:131072 super:16777216 boot_a:131072 init_boot_a:16384 vendor_boot_a:131072 frp:2048; do
  n=${spec%%:*}; s=${spec##*:}; part "$n" "$s"
  sgdisk -n $i:$START:$END -c $i:$n -t $i:8300 "$OUT" >/dev/null
  START_OF[$n]=$START; IDX_OF[$n]=$i; i=$((i+1))
done
# The kernel partition GUID lets Android find its boot disk on any controller (USB/SATA/NVMe)
BOOT_GUID=$(sgdisk -i ${IDX_OF[boot_a]} "$OUT" | awk '/Partition unique GUID/{print tolower($4)}')

# userdata takes the rest of the disk
sgdisk -n $i:$off:0 -c $i:userdata -t $i:8300 "$OUT" >/dev/null

w() { local img=$1 sector=$2; local len; len=$(stat -c %s "$img")
      fallocate -p -o $((sector*512)) -l "$len" "$OUT"
      dd if="$img" of="$OUT" bs=512 seek="$sector" conv=notrunc,sparse status=none; }
w "$P/vbmeta.img"              ${START_OF[vbmeta_a]}
w "$P/vbmeta_system.img"       ${START_OF[vbmeta_system_a]}
w "$P/vbmeta_vendor_dlkm.img"  ${START_OF[vbmeta_vendor_dlkm_a]}
w "$P/vbmeta_system_dlkm.img"  ${START_OF[vbmeta_system_dlkm_a]}
w "$P/boot.img"                ${START_OF[boot_a]}
w "$P/init_boot.img"           ${START_OF[init_boot_a]}
w "$P/vendor_boot.img"         ${START_OF[vendor_boot_a]}
"$H/simg2img" "$P/super.img" "$W/super.raw"
w "$W/super.raw"               ${START_OF[super]}

# EFI system partition: FAT32 inside the image, GRUB standalone + kernel + initrd + grub.cfg
dd if=/dev/zero of="$W/esp.img" bs=1M count=512 status=none
mkfs.vfat -F 32 -n EFI "$W/esp.img" >/dev/null
THEME_DIR=${THEME_DIR:-$(dirname "$0")/../../boot/grub-theme/out}
if [ ! -d "$THEME_DIR" ] && [ -x "$(dirname "$0")/../../boot/grub-theme/gen-theme.sh" ]; then "$(dirname "$0")/../../boot/grub-theme/gen-theme.sh" "$THEME_DIR" >/dev/null; fi
LOADFONTS=""
for f in "$THEME_DIR"/noto_medium_20.pf2 $(ls "$THEME_DIR"/*.pf2 | grep -v noto_medium_20); do [ -e "$f" ] && LOADFONTS+="loadfont /theme/$(basename "$f")"$'\n'; done
cat > "$W/grub.cfg" <<EOF
set timeout=${GRUB_TIMEOUT:-3}
set default=0
# the standalone GRUB starts on its memdisk: switch to the EFI partition that holds kernel+initrd
search --no-floppy --label --set=root EFI

# Material You theme (boot/grub-theme)
insmod all_video
insmod gfxterm
insmod gfxmenu
insmod png
set gfxmode=${GFXMODE:-1280x800,1024x768,auto}
set gfxpayload=keep
${LOADFONTS}set gfxterm_font="Noto Sans Medium 20"
terminal_output gfxterm
set theme=/theme/theme.txt
menuentry "Android (x86_64)" {
    linux /kernel console=tty0 earlyprintk=efi printk.devkmsg=on audit=1 panic=-1 cma=0 firmware_class.path=/vendor/etc/ loop.max_part=7 init=/init \
        androidboot.hardware=cutf_cvm androidboot.fstab_suffix=cf.ext4.cts androidboot.slot_suffix=_a androidboot.verifiedbootstate=orange \
        androidboot.force_normal_boot=1 androidboot.selinux=permissive androidboot.boot_part_uuid=$BOOT_GUID $CMDLINE_EXTRA
    initrd /initrd.img
}
EOF
grub-mkstandalone -O x86_64-efi -o "$W/BOOTX64.EFI" --compress=xz --modules="part_gpt part_msdos fat search search_label search_fs_file search_fs_uuid normal linux echo efi_gop video all_video gfxterm gfxmenu gfxterm_background png font gfxterm_menu" "boot/grub/grub.cfg=$W/grub.cfg" >/dev/null 2>&1
mmd -i "$W/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$W/esp.img" "$W/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp.img" "$C/boot/kernel" ::/kernel
mcopy -i "$W/esp.img" "$C/initrd.img" ::/initrd.img
mcopy -i "$W/esp.img" "$W/grub.cfg" ::/grub.cfg
if [ -d "$THEME_DIR" ]; then mmd -i "$W/esp.img" ::/theme; mcopy -i "$W/esp.img" "$THEME_DIR"/* ::/theme/; fi
dd if="$W/esp.img" of="$OUT" bs=512 seek=$ESP_START conv=notrunc status=none
echo "image: $OUT ($(du -sh "$OUT" | cut -f1) on disk)"
sgdisk -p "$OUT" | tail -16
