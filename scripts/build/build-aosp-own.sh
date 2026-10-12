#!/bin/bash
. /mnt/storage-adata/aosp-env.sh
export SYSTEM_DLKM_SRC=vendor/ours/kernel/system_dlkm
export KERNEL_MODULES_PATH=vendor/ours/kernel/vendor_dlkm
cd /mnt/storage-adata/aosp15
set +u
source build/envsetup.sh >/dev/null 2>&1
lunch ours_cf_x86_64_phone-trunk_staging-userdebug || exit 3
export CF_VENDOR_NO_UWB=true CF_VENDOR_NO_THREADNETWORK=true LOCAL_ENABLE_OEMLOCK=false
nice -n 5 m -j4 > build.log 2>&1
echo "exit=$?" >> build.log
