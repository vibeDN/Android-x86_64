#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Host tests of the NativeBridge: builds the guest root + arm64 test lib, runs the fake-ART driver.
# usage: run_tests.sh <cmake build dir of translator/bridge>
set -eu
B=$(realpath "$1"); HERE=$(cd "$(dirname "$0")" && pwd)
G=${GUEST_ROOT:-$B/guest-root}; L=$B/testlibs
"$HERE/../guest/build_guest.sh" "$G" >/dev/null
GUEST_LIBLOG=$G/system/lib64/armtr/liblog.so "$HERE/build_test_lib.sh" "$L"
cp -f ${TESTS_OUT:-/mnt/storage-adata/translator-research/tests-out-ours}/libjni_test.so "$L/"
ARMTR_SYSROOT=$G ARMTR_HOSTLIBS_DIR=$B/fakelibs timeout 120 "$B/bridge_host_test" "$B/libarmtr_bridge.so" "$L"
