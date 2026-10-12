import os, glob, shutil

A = "/mnt/storage-adata/aosp15"
orig_sys = sorted(os.path.basename(p) for p in glob.glob(A + "/kernel/prebuilts/6.12/x86_64/*.ko"))
orig_vnd = sorted(os.path.basename(p) for p in glob.glob(A + "/kernel/prebuilts/common-modules/virtual-device/6.12/x86-64/*.ko"))
ours = {}
for p in glob.glob("/mnt/storage-adata/kernel/modstage/lib/modules/*/**/*.ko", recursive=True):
    ours[os.path.basename(p)] = p

SYS = A + "/vendor/ours/kernel/system_dlkm"
VND = A + "/vendor/ours/kernel/vendor_dlkm"
for d in (SYS, VND):
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)

def fill(names, dst):
    miss = []
    for n in names:
        if n in ours:
            shutil.copy(ours[n], dst + "/" + n)
        else:
            miss.append(n)
    return miss

ms = fill(orig_sys, SYS)
mv = fill(orig_vnd, VND)
shutil.copy("/mnt/storage-adata/kernel/out-cf/arch/x86/boot/bzImage", SYS + "/kernel-6.12")
print("system: want %d have %d" % (len(orig_sys), len(orig_sys) - len(ms)))
print("  missing:", " ".join(ms))
print("vendor: want %d have %d" % (len(orig_vnd), len(orig_vnd) - len(mv)))
print("  missing:", " ".join(mv))
extra = [n for n in ours if n not in orig_sys and n not in orig_vnd]
print("our modules not in either original list: %d" % len(extra))
