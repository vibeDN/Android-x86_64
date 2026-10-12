#!/bin/bash
# write freshly built AOSP images into the GPT test disk (offsets in 512B sectors)
P=/mnt/storage-adata/aosp15/out/target/product/vsoc_x86_64; H=/mnt/storage-adata/aosp15/out/host/linux-x86/bin
C=/mnt/storage-adata/gsi16/cf; D=$C/disk.img
w(){ # punch a hole over the old contents first, so sparse writes cannot leave stale blocks
  len=$(stat -c %s "$1"); fallocate -p -o $(($2*512)) -l $len $D; dd if="$1" of=$D bs=512 seek=$2 conv=notrunc,sparse status=none; }
w $P/vbmeta.img 4096; w $P/vbmeta_system.img 6144; w $P/vbmeta_vendor_dlkm.img 8192; w $P/vbmeta_system_dlkm.img 10240
w $P/boot.img 29503488; w $P/init_boot.img 29634560; w $P/vendor_boot.img 29650944
$H/simg2img $P/super.img $C/super.raw && w $C/super.raw 143360; rm -f $C/super.raw
