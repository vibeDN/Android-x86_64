#!/bin/bash
# Boot the UEFI PC image in QEMU with OVMF; disk is attached as NVMe to prove controller independence.
C=/mnt/storage-adata/gsi16/cf; IMG=${1:-/mnt/storage-adata/gsi16/android-pc.img}
VARS=$C/OVMF_VARS.fd; [ -f $VARS ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd $VARS
exec qemu-system-x86_64 -enable-kvm -cpu host -m 4G -smp 4 -display none -no-reboot \
 -machine q35 \
 -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=$VARS \
 -drive file=$IMG,format=raw,if=none,id=hd0,cache=unsafe -device nvme,drive=hd0,serial=androidpc \
 -chardev socket,id=ser0,path=$C/console.sock,server=on,wait=off,logfile=$C/serial-pc.log -serial chardev:ser0 -monitor none -qmp unix:$C/qmp.sock,server=on,wait=off \
 -device ramfb,id=gpu0 -device virtio-keyboard-pci -device virtio-mouse-pci -device virtio-rng-pci \
 -device vhost-vsock-pci,guest-cid=3 \
 -device virtio-serial-pci -chardev file,id=h0,path=$C/hvc0.log -device virtconsole,chardev=h0 -chardev file,id=h1,path=$C/hvc1.log -device virtconsole,chardev=h1 -chardev file,id=h2,path=$C/logcat.log -device virtconsole,chardev=h2 \
 $(for i in 3 4 5 6 7 8 9 10; do echo "-chardev null,id=n$i -device virtconsole,chardev=n$i"; done)
