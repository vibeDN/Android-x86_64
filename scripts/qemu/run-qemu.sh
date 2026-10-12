#!/bin/bash
# usage: run-qemu.sh <kernel> [extra cmdline]
C=/mnt/storage-adata/gsi16/cf
K=${1:-$C/boot/kernel}; shift
exec qemu-system-x86_64 -enable-kvm -cpu host -m 3G -smp 4 -nographic -no-reboot -serial unix:$C/console.sock,server=on,wait=off -monitor none \
 -kernel $K -initrd ${INITRD:-$C/initrd.img} \
 -drive file=$C/disk.img,format=raw,if=none,id=hd0,cache=unsafe -device virtio-blk-pci,drive=hd0,addr=0x12 \
 -device virtio-serial-pci,addr=0x11 -chardev file,id=h0,path=$C/hvc0.log -device virtconsole,chardev=h0 -chardev file,id=h1,path=$C/hvc1.log -device virtconsole,chardev=h1 -chardev file,id=h2,path=$C/logcat.log -device virtconsole,chardev=h2 -chardev null,id=n3 -device virtconsole,chardev=n3 -chardev null,id=n4 -device virtconsole,chardev=n4 -chardev null,id=n5 -device virtconsole,chardev=n5 -chardev null,id=n6 -device virtconsole,chardev=n6 -chardev null,id=n7 -device virtconsole,chardev=n7 -chardev null,id=n8 -device virtconsole,chardev=n8 -chardev null,id=n9 -device virtconsole,chardev=n9 -chardev null,id=n10 -device virtconsole,chardev=n10 -device virtio-rng-pci -device virtio-gpu-pci,id=gpu0,addr=0x10 -qmp unix:$C/qmp.sock,server=on,wait=off -device virtio-keyboard-pci,id=kbd0 -device virtio-mouse-pci,id=mouse0 -device vhost-vsock-pci,guest-cid=3 \
 -append "console=ttyS0 earlyprintk=serial,ttyS0 printk.devkmsg=on audit=1 panic=-1 8250.nr_uarts=1 cma=0 firmware_class.path=/vendor/etc/ loop.max_part=7 init=/init androidboot.hardware=cutf_cvm androidboot.fstab_suffix=cf.ext4.cts androidboot.slot_suffix=_a androidboot.verifiedbootstate=orange androidboot.boot_devices=pci0000:00/0000:00:12.0 androidboot.console=ttyS0 androidboot.selinux=permissive $*"
