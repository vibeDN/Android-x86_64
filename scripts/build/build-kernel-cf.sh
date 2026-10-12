#!/bin/sh
echo 500 > /proc/self/oom_score_adj
cd /mnt/storage-adata/kernel/common
A="LLVM=1 O=../out-cf ARCH=x86_64 HOSTRUSTC=/mnt/storage-adata/aosp15/prebuilts/rust/linux-x86/1.88.0/bin/rustc RUSTC=/mnt/storage-adata/aosp15/prebuilts/rust/linux-x86/1.88.0/bin/rustc BINDGEN=/mnt/storage-adata/aosp15/prebuilts/clang-tools/linux-x86/bin/bindgen RUST_LIB_SRC=/mnt/storage-adata/aosp15/prebuilts/rust/linux-x86/1.88.0/lib/rustlib/src/rust/library"
nice -n 10 make $A olddefconfig > ../build-cf.log 2>&1
nice -n 10 make $A -j8 bzImage modules >> ../build-cf.log 2>&1
echo "exit=$?" >> ../build-cf.log
rm -rf /mnt/storage-adata/kernel/modstage; mkdir -p /mnt/storage-adata/kernel/modstage
nice -n 10 make $A INSTALL_MOD_PATH=/mnt/storage-adata/kernel/modstage INSTALL_MOD_STRIP=1 modules_install >> ../build-cf.log 2>&1
echo "install_exit=$?" >> ../build-cf.log
