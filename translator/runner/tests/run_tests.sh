#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# usage: run_tests.sh <armtr-run binary> [builddir]   (M1 static, M2 bionic dynamic; compares with qemu-aarch64 if present)
set -u
RUN=$(realpath "$1"); B=${2:-$(dirname "$RUN")}; HERE=$(cd "$(dirname "$0")" && pwd)
S=${SYSROOT:-/mnt/storage-adata/translator-research/sysroot}; T=${TESTS_OUT:-/mnt/storage-adata/translator-research/tests-out}
CL=$(ls -d /mnt/storage-adata/aosp15/prebuilts/clang/host/linux-x86/clang-r563880/bin/clang)
fail=0; ok() { echo "PASS $1"; }; bad() { echo "FAIL $1"; fail=1; }
$CL --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld -O1 -fno-stack-protector $HERE/hello_static.c -o $B/hello_static || exit 1
out=$($RUN $B/hello_static a b 2>&1); rc=$?
[ $rc = 42 ] && echo "$out" | grep -q "argc=0x0000000000000003" && echo "$out" | grep -q "tbi=0x000000000000004d" && ok M1-static || bad M1-static
out=$($RUN --sysroot $S $T/hello_dyn x y 2>/dev/null); [ "$out" = "hello bionic arm64 pid>0=1 argc=3" ] && ok M2-hello_dyn || bad M2-hello_dyn
$RUN --sysroot $S $T/stress_dyn 2>/dev/null > $B/stress.out; rc=$?
cat > $B/stress.golden <<'G'
aes    95af22c681e1c8a5
sha    4fdb2807a6f75f96
pmull  ea7cc9df68107008 00962335c6bedea6
crc32  02ff48c1
neon   332281960 332281960 250.749985 332834.468750
double 2993.412231619840
fcvt   -3 1000000000000000000 3900000000 16777216.0
G
[ $rc = 0 ] && cmp -s $B/stress.out $B/stress.golden && ok M2-stress_dyn || bad M2-stress_dyn
out=$(timeout 60 $RUN --sysroot $S $T/pthread_test_dyn 2>/dev/null); rc=$?
[ $rc = 0 ] && [ "$out" = "counter=80000 atomic=80000 tls_bad=0 main_tls=0" ] && ok M2-pthread_test_dyn || bad M2-pthread_test_dyn
out=$(cd $T && $RUN --sysroot $S ./jni_host_dyn 2>/dev/null); rc=$?
[ $rc = 0 ] && [ "$out" = "onload=0x10006 n=1 name=add sig=(II)I add(5,7)=22 add2=23" ] && ok M2-jni_host_dyn || bad M2-jni_host_dyn
exit $fail
