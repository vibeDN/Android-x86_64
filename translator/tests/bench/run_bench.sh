#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run the benchmark set (build_bench.sh) N times per runner and print the median ms per benchmark.
# usage: run_bench.sh OUT N name=CMD...    CMD gets the arm64 (or x86_64 for name "native") binary appended,
#   e.g. run_bench.sh /mnt/storage-adata/tmp/bench-out 3 native= qemu="qemu-aarch64 -L $S" armtr="armtr-run --sysroot $S"
# Checksums are compared against the first runner; a mismatch is printed as "!".
set -u
OUT=$1; N=$2; shift 2
DB=${DB:-$OUT/bench.db}
declare -A ms ck; names=(); benches=()
for spec in "$@"; do
  name=${spec%%=*}; cmd=${spec#*=}; names+=("$name")
  arch=arm64; [ "$name" = native ] && arch=x86_64
  for i in $(seq "$N"); do
    while read -r b t c; do
      [[ " ${benches[*]} " == *" $b "* ]] || benches+=("$b")
      ms[$name,$b]+="$t "; ck[$name,$b]=$c
    done < <($cmd $OUT/$arch/bench 2>/dev/null; $cmd $OUT/$arch/dbz_bench $DB 2>/dev/null)
  done
done
printf "%-8s" bench; printf " %10s" "${names[@]}"; echo
for b in "${benches[@]}"; do
  printf "%-8s" "$b"
  for n in "${names[@]}"; do
    med=$(echo ${ms[$n,$b]:-} | tr ' ' '\n' | sort -n | awk '{a[NR]=$1} END {print NR ? a[int((NR+1)/2)] : "-"}')
    flag=""; [ "${ck[$n,$b]:-}" != "${ck[${names[0]},$b]:-}" ] && flag="!"
    printf " %10s" "$med$flag"
  done
  echo
done
