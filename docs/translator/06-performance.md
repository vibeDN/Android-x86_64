# 06 - armtr performance: benchmarks, bottlenecks, fixes

Date: 2026-10-13. Host: 12-core x86_64, Debian 13, Linux 6.12, 15 GB RAM. All numbers are wall-clock ms measured
in the guest with `clock_gettime(CLOCK_MONOTONIC)` (startup is excluded unless noted). Each value is the median of
3 runs (base and softmmu columns: 1 run). Variance is about +-5%.

## Benchmark set

Sources are in `translator/tests/bench/`.
- `build_bench.sh [OUT]` builds two programs for arm64 bionic (AOSP clang, flags as `tests/build.sh`) and natively
  for x86_64 glibc: `bench` and `dbz_bench`. sqlite (3.50.4) and zlib are compiled from AOSP `external/` (read only).
- `run_bench.sh OUT N name=CMD...` prints the median per benchmark. It marks a checksum that differs from the first
  runner with `!`. All runners produce identical checksums.

| name | what | stresses |
|---|---|---|
| sha256 | plain-C SHA-256 of 16 MiB | integer ALU, loads |
| lz | toy LZ77 with a hash table, 3 x 8 MiB | byte loads/stores, branches |
| call | 30 M indirect calls through a function-pointer table | `blr`/`ret`, TB lookup |
| mem | 20 x memset + memcpy of 32 MiB | streaming memory, DC ZVA |
| malloc | 2 M random free/malloc (16..2064 B) | scudo: atomics, crc32 checksums, DC ZVA |
| neon | auto-vectorized f32 saxpy/dot + u8 SAD, 64 Ki elements x 1000 | NEON float/int |
| mutex | 50 k condvar ping-pong round trips, 2 threads | futex, cross-thread wakeups |
| atomic | 4 threads x 200 k contended mutex + `atomic_fetch_add` | LSE/exclusives, futex |
| syscall | 300 k one-byte `write("/dev/null")` | svc path |
| pipe | 200 k write+read round trips on a pipe | svc path |
| sqlite | TDLib-like: WAL file db, 100 k inserts in 100 transactions, index, 2000 indexed selects, a LIKE scan | realistic mixed code, calls, syscalls |
| zlib | deflate level 6 of 8 MiB + 4 x inflate + crc32 | realistic tight loops |

Runners:
- native = the x86_64 build;
- qemu = `qemu-aarch64` 10.0 (Debian) `-L sysroot`;
- base = armtr before this work (Unicorn 2.1.4 + the CF_PARALLEL/xchg patches);
- softmmu = all fixes except direct memory (`ARMTR_SOFTMMU=1`);
- after = all fixes.

Sysroot: `/mnt/storage-adata/translator-research/sysroot`.

## Results

| bench | native | qemu-aarch64 | armtr base | armtr softmmu | **armtr after** | after vs base | after vs qemu |
|---|---:|---:|---:|---:|---:|---:|---:|
| sha256 | 55 | 209 | 1236 | 252 | **193** | 6.4x | 1.08x faster |
| lz | 115 | 245 | 968 | 391 | **233** | 4.2x | 1.05x faster |
| call | 36 | 579 | 437 | 283 | **292** | 1.5x | 2.0x faster |
| mem | 59 | 149 | 5670 | 284 | **128** | 44x | 1.16x faster |
| malloc | 93 | 1042 | 52773 | 2099 | **838** | 63x | 1.24x faster |
| neon | 43 | 808 | 2866 | 1577 | **998** | 2.9x | 1.23x slower |
| mutex | 163 | 200 | 200 | 185 | **182** | 1.1x | 1.10x faster |
| atomic | 62 | 229 | 1304 | 204 | **141** | 9.3x | 1.63x faster |
| syscall | 29 | 81 | 53 | 57 | **54** | 1.0x | 1.50x faster |
| pipe | 58 | 133 | 90 | 94 | **89** | 1.0x | 1.48x faster |
| sqlite | 902 | 4698 | 31765 | 5259 | **2811** | 11.3x | 1.67x faster |
| zlib | 449 | 1325 | 8423 | 2427 | **1019** | 8.3x | 1.30x faster |

Whole programs (process wall time, median of 5) and engine microbenchmarks (`engine/tests/bench_engine`):

| | qemu-aarch64 | armtr base | armtr after |
|---|---:|---:|---:|
| hello_dyn (startup: linker64 + libc init) | 38 ms | 37 ms | 27 ms |
| stress_dyn | 41 ms | 41 ms | 29 ms |
| pthread_test_dyn (4 threads, mutex + atomics) | 79 ms | 183 ms | 49 ms |
| `armtr_call` round trip (host -> guest -> host) | - | 3090 ns | 159 ns |
| `armtr_call` + 1 svc / + 1 trampoline | - | 3335 / 3455 ns | 222 / 218 ns |
| tight `subs/b.ne` loop | - | 1518 M insn/s | 1894 M insn/s |

Compared with native x86_64, armtr now ranges from 1.1x slower (futex-bound code) to about 3x slower (sqlite,
mem, lz, sha256, zlib) and 8-9x slower (malloc, call). NEON float is about 23x slower.

## What was slow and what fixed it (in order of impact)

The profiles below come from `perf record -g` on the host against `armtr-run`. Unicorn has no frame pointers,
so the flat profile (`--no-children --sort dso,sym`) was the useful view. uprobes on `tb_gen_code` and
`do_tb_phys_invalidate` found the `armtr_call` problem. Each step below was measured on the full set; the
intermediate numbers are rounded.

1. **Every guest store took the self-modifying-code slow path (base malloc 54 s, mem 5.7 s).**
   - Profile: 75% in `notdirty_write` -> `tb_invalidate_phys_page_fast` / `flatview_translate` / `find_memory_mapping`.
   - Unicorn stubs QEMU's dirty bitmap, so every TLB fill sets `TLB_NOTDIRTY`.
   - `notdirty_write` only clears it when the page is not executable in the TLB. Our whole window is mapped RWX,
     so that never happens.
   - Fix (Unicorn): set NOTDIRTY only on pages that really hold TBs (`tb_page_has_code`), and clear it once the
     last TB on the page is gone. `tb_gen_code` re-protects the page even when mem hooks exist; it used to skip
     that when our UNMAPPED hook was installed, which broke SMC detection.
   - Result: malloc 54 s -> 2.9 s, sqlite 34 s -> 9.7 s, mem 5.5 s -> 0.62 s.
2. **A helper call after every guest load/store (Unicorn's `check_exit_request`) and at every TB start.**
   - Fix: `uc->no_exit_request` (engine). Unmapped accesses already siglongjmp out, and kicks only need a TB
     boundary.
   - Fix: upstream QEMU's inline `icount_decr` test at TB start, replacing the helper (Unicorn patch).
   - Result: sqlite 9.7 -> 6.1 s, zlib 3.6 -> 2.1 s.
3. **`cpu_restore_state()` on every TLB fill.** Unicorn's virtual-TLB fill does a TB search plus re-decode per
   miss so that the hook could read pc. Our hook does not read pc, so the call was removed. The unmapped and
   miss paths still sync pc themselves.
4. **The softmmu TLB itself (direct memory mode).**
   - Even with the fixes above, softmmu cost about 30% on memory-heavy code: inline TLB compares, 256-entry TLB
     misses, and `tlb_set_page` per page per pass.
   - Guest VA == host VA, so the x86 backend now emits plain host loads/stores, as linux-user does with
     `guest_base=0` (`uc->armtr_direct_mem`).
   - TBI moved into the translator: TCR_EL1.TBI0/1 is set and `clean_data_tbi` sign-extends from bit 55, as QEMU
     user mode does.
   - Faults arrive as host SIGSEGV. The engine already mapped those to exact guest faults (`cpu_restore_state`
     from the host pc). The null page and other Unicorn-unmapped addresses now take the same path. Messages:
     "unmapped/protected read/write/fetch".
   - Result: sqlite 6.1 -> 3.7 s, mem 291 -> 151 ms, lz 398 -> 256 ms.
5. **No TB chaining across guest pages.**
   - System-mode QEMU links TBs only within a page, because a VA->PA mapping can change underneath. Ours cannot,
     and invalidating a TB unlinks its incoming jumps.
   - So `use_goto_tb` links across pages in direct mode, as linux-user does.
   - Result: zlib 1876 -> 1043 ms, sqlite 3.7 -> 2.8 s. `helper_lookup_tb_ptr` + `cpu_get_tb_cpu_state` dropped
     from 36% to 15% of sqlite/zlib.
6. **scudo-specific helpers (malloc 1404 -> 828 ms).**
   - DC ZVA becomes a plain `memset` in direct mode; its always-passing access-check helper is skipped.
   - Atomics (`atomic_mmu_lookup`) use the host address directly. Alignment checks are kept.
   - QEMU's table-driven `crc32c` now uses the SSE4.2 `crc32` instruction. scudo checksums every chunk header
     with `crc32cx`, and Android x86_64 requires SSE4.2.
   - The per-TB `tlb_reset_dirty_by_vaddr` (a scan of the whole TLB per translation) is skipped in direct mode.
7. **`armtr_call` re-translated the exit TB every time (3.1 us -> 0.16 us).**
   - After each `uc_emu_start`, Unicorn invalidates the zero-size "halt here" TB at the `until` address, and the
     next call re-translates it.
   - The outermost run's exit TB is now kept cached. `uc_emu_start` drops it as soon as a run uses a different
     exit; nested runs keep the old behaviour.
   - This is the JNI host->guest call cost (docs 01 listed 3.8 us as a risk).

The svc path was already cheap (the syscall/pipe benchmarks beat qemu). Futex contention costs what the host
costs.

Things that turned out not to matter:
- CF_PARALLEL atomics already use host atomics (`cmpxchg`, CMPXCHG128 available). No `start_exclusive` was seen
  in any profile.
- `armtr_mem_*` invalidations were rare. Now only ranges becoming executable are logged.
- No global UC_HOOK_CODE/BLOCK hooks are installed.

## Semantics that changed (all covered by tests)

- **Self-modifying code needs `IC IVAU`.** In direct mode a guest store bypasses Unicorn's notdirty tracking.
  - `IC IVAU` (what `__clear_cache` emits, and what real arm64 requires anyway) calls `on_ic_ivau`. That drops
    the line's TBs on this cpu and logs the range for the others.
  - mmap/mprotect with PROT_EXEC is logged as before.
  - Every cpu now applies the log at its **next syscall** as well as at `armtr_run` entry. Before, a long-running
    thread never saw invalidations. This fixes dlclose + dlopen at the same address.
  - Map/unmap/protect of non-exec ranges no longer invalidate anything.
  - Test: `test_engine` "self-modifying code" (`F_STORE_CODE`).
- Unaligned atomics no longer raise an alignment fault in direct mode. Unaligned exclusives are not checked
  either, as in linux-user QEMU.
- An access to a kernel-half (bit 55 set) address faults on the host as a non-canonical #GP. The guest sees
  SIGSEGV with si_addr 0 instead of the address. Only buggy code does this.
- `armtr_run` stop addresses must not be executed as code by a nested run that uses a different stop address.
  All callers use 0x1000 or the cpu's stack guard page, so this holds.
- `ARMTR_SOFTMMU=1` restores the TLB path (with fixes 1-3 and 7) for debugging. Both modes pass all tests.

## Files

- `scripts/unicorn-armtr-perf.patch`: about 100 lines in 14 Unicorn files. `scripts/fetch-unicorn.sh` applies it
  idempotently, and it is required because the engine sets its new `uc_struct` fields.
- `translator/engine/engine.cc`, `uc_repair.c` (`armtr_uc_tune`).
- `engine/tests/{test_engine.cc,guest.S,common.h}`.
- `translator/tests/bench/*`.
- `engine/CMakeLists.txt` now defaults to the in-tree Unicorn, like the runner and bridge.

Tests run: engine/loader/syscalls/proxy/hostlibs ctest, `runner/tests/run_tests.sh` (M1-M3 incl. signals), and
`bridge/tests/run_tests.sh`. All pass in both modes.

## Remaining hotspots (after)

- sqlite/zlib:
  - 67% in translated code.
  - About 15% in `helper_lookup_tb_ptr` + `cpu_get_tb_cpu_state` (every `ret`/`br`/`blr`).
  - About 11% in the kernel (file I/O).
  - Next step: an inline jump-cache probe in TCG, or a return-address stack.
- neon: about 50% in softfloat helpers. Hardfloat is active (`mulss`), but QEMU 5 calls one helper per lane per
  op. This is the only benchmark where qemu 10 still wins.
- Per-thread warm-up: each guest thread has its own engine and translation cache, so a new thread re-translates
  everything it runs. Short-lived thread pools pay for this. Unicorn cannot share TBs between engines.

## Recommendation: dynarmic engine?

**Not now; keep Unicorn as the engine and schedule a time-boxed dynarmic spike only when real app traces show
CPU-bound guest code (NEON/float-heavy game or codec code) dominating.**

Why not now:
- With these fixes armtr beats qemu-aarch64 10 on 11 of 12 workloads, by 1.7x on the realistic sqlite one.
- The JNI call cost dropped 20x.
- What is left is mostly inherent to TCG: indirect-branch lookups and helper-per-lane floats.

What dynarmic would bring: a register-allocating A64->x64 JIT, host SIMD for most NEON float ops, a return stack
buffer for `ret`, and fastmem. Expect the neon/call class of code to improve several-fold, and smaller gains
elsewhere. That would be measured in the spike, not assumed.

What it would take (behind `engine.h`, about 4-6 weeks for parity, 1-2 weeks for a spike that runs `bench`):
- **Build**: dynarmic plus mcl, xbyak, zydis, fmt, robin-map (0BSD/BSD/MIT), via CMake on the host and an
  Android.bp later. The engine can then drop the GPL requirement.
- **Memory**: `fastmem_pointer = 0` (identity) plus `page_table`. Dynarmic's fastmem uses its own SIGSEGV
  backpatching handler (`exception_handler_posix.cpp`), which has to be chained with ours and ART's in a defined
  order.
- **Precise faults**: dynarmic has no `cpu_restore_state`. A guest SIGSEGV at an exact pc needs
  `recompile_on_fastmem_failure` plus memory callbacks that raise the guest fault. Our signal frames (docs 04)
  depend on exact pc and registers.
- **Gaps found in the checked-out source**:
  - ARMv8.1 LSE atomics (CAS*, LDADD*, SWP*) are commented out in `frontend/A64/decoder/a64.inc`. We would have to
    stop advertising HWCAP_ATOMICS, so bionic's outline atomics fall back to LL/SC. Apps built with
    `-march=armv8.1-a` or later would break until that is implemented.
  - `ExclusiveMonitor(processor_count)` is sized at creation, but Android processes create threads without bound.
    It needs a patch or one monitor slot per live thread.
- **Engine glue**: `CallSVC` maps to our svc hook, trampolines become an svc, `tpidr_el0` is a config pointer,
  kicks map to `HaltExecution`, and invalidation maps to `InvalidateCacheRange`.
- **Validation**: all of runner M1-M3, the bridge tests and the benchmark checksums. Keep Unicorn selectable at
  runtime as the reference engine.

Note: azahar's fork targets 3DS (A32). For A64 maintenance, compare against the yuzu-lineage forks before
picking one.
