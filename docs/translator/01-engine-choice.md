# 01 - ARM64 -> x86_64 translator: engine choice

Date: 2026-10-12. Status: research memo + host PoC. Evidence lives in /mnt/storage-adata/translator-research/.

## Decision
**Start with Unicorn 2.1.4 (GPLv2) behind a thin `GuestCpu` interface (about 10 calls).**
Use it in a flat identity-mapped mode: guest VA == host VA, mapped with `uc_mem_map_ptr`.
Keep two escape hatches: **dynarmic A64** (0BSD, x86_64 JIT, evaluated below but not built) and a **QEMU linux-user extraction** (the "full speed" fallback).

Why Unicorn first:
- It is the only candidate that is "QEMU TCG as a library" today. It has a stable C API and no glib dependency.
- It builds in 21 s with `-j2`, and aarch64 is selectable (`-DUNICORN_ARCH=aarch64`).
- The flat-memory approach is verified (see PoC).
- The GPL piece stays isolated in its own directory, as you intended.

## Evidence
### 1. Unicorn
- Latest tag is v2.1.4 (Sep 2025). Its QEMU base is **5.0.1** (`qemu/VERSION`), so it lacks 5 years of TCG and aarch64 work.
  - aarch64 CPU models are A57/A53/A72/MAX. MAX should expose LSE atomics and crypto, but this is **unverified**.
  - Check that bionic's arm64 ifunc/cpu-feature probing and any ARMv8.2+ instructions (dotprod, SVE) work.
- glib: `glib_compat/` is bundled (CMakeLists.txt L1181+). There is no host glib dependency.
- Android: CMakeLists.txt already has `ANDROID_ABI` branches (L185, L319, L1247, L1428). An x86_64 NDK-style or AOSP clang build should work.
  - **Not tested** against the bionic/AOSP clang toolchain. Host glibc build only.
  - It needs pthread, and may need `-fPIC` and the soong cc_library wrapper.
- Memory model:
  - `uc_mem_map` allocates and copies, which is unusable for us.
  - `uc_mem_map_ptr(uc, addr, size, perms, host_ptr)` shares host memory with no copy.
  - Verified: guest wrote host-owned memory and read host-prepared data.
  - Verified: a **single 64 TiB region** `uc_mem_map_ptr(0x10000, 1<<46, ..., ptr=0x10000)` is accepted, with no per-mmap bookkeeping.
  - Consequence: JNI pointers, Java arrays (once pinned) and guest `mmap` all just work. There is no sync and no remapping.
  - Gap: accesses to unmapped host pages raise a host SIGSEGV inside the TCG helper, not a guest fault.
    - We need a SIGSEGV handler. It must sigsetjmp/longjmp out of `uc_emu_start`, or chain to ART's handler.
    - ART's handler uses SIGSEGV for implicit null checks and stack overflow, so handler ordering matters.
- Softmmu: Unicorn keeps the QEMU softmmu TLB path, so every load/store goes through the TLB fast path.
  - It does not use the linux-user `g2h` direct-access path.
  - Expect roughly 1.5-3x slower than qemu-user on memory-heavy code (my estimate, not measured here).
- Hooks: `UC_HOOK_INTR` receives SVC (used in the PoC), plus `UC_HOOK_MEM_*`, `UC_HOOK_INSN_INVALID` and `uc_ctl_flush_tb`.
- Threads: one `uc_engine` per guest thread (separate instances are independent). A single engine is not thread-safe.
  - Per-thread engines with identity-mapped shared memory are fine.
  - Atomics and exclusives (LDXR/STXR) run on the host through TCG helpers, so they are coherent across engines in theory. Test this.
  - Context cost: `uc_context_save/restore`, or one engine per thread (about 1 MB+ of TCG buffer each; tunable with `uc_ctl_set_tcg_buffer_size`).
- No translated-code cache sharing between engines. Per-engine JIT warm-up is a startup cost; bionic libc + ART startup is heavy.
- No linux-user layer: syscalls, signals, clone, TLS (TPIDR_EL0) and the loader are ours to write. We need that anyway for a native bridge (guest libc/linker, syscall number and struct translation).

### 2. Extracting QEMU linux-user + TCG
- Project search found no maintained "libqemu-user" or native-bridge-from-qemu-user project. The closed-source libhoudini/ndk_translation is the only real equivalent.
- Upside:
  - Modern QEMU (10.0 is installed on the host and runs aarch64 static binaries).
  - Direct `g2h` memory access with `guest_base=0` (no TLB) is the fastest QEMU mode.
  - It brings signals, syscalls, clone/TLS and the ELF loader.
- Blockers:
  - It is a process-singleton design: global `cpu_list`, `main()` owns the setup, `exit_group` kills the process, and it installs its own SIGSEGV/SIGBUS handlers (needed for self-modifying-code write protection).
  - It needs glib, an old-style meson build, and a large dependency surface; not a Soong build.
  - It needs a rewrite to embed in ART.
- Effort: 6-10 weeks for a first working extraction, then permanent merge cost against upstream. Only worth it if Unicorn proves too slow.

### 3. Others
- **dynarmic** (https://github.com/merryhime/dynarmic; the azahar-emu fork was last committed 2026-09-26, so it is alive):
  - Real A64 -> x86_64 in-process JIT, license 0BSD (no GPL needed).
  - Checked `config.h`: `page_table`/`absolute_offset_page_table` plus fastmem pointer (flat identity mapping is natural), `CallSVC`, an `ExclusiveMonitor`, and `backend/x64`.
  - Deps: mcl, oaknut, xbyak, fmt, robin-map, zydis. **Not built** (kept within the cheap budget).
  - Unverified risks: LSE and SVE coverage, SIMD completeness, and how fastmem's SIGSEGV backpatching coexists with ART's handler.
  - It is the strongest "second engine" candidate and likely faster than Unicorn on hot loops.
- Berberis: no arm64 guest in our tree (confirmed). Its runtime structure is the right template for the bridge side (`native_bridge`, `guest_loader`, `guest_os_primitives`).
- Closed: libhoudini, ndk_translation (Google/Intel). Rosetta, FEX and box64 go the wrong direction.

## PoC (done, host glibc x86_64)
Files: /mnt/storage-adata/translator-research/unicorn/{poc.S,poc.c,big.c}. Library built at build/libunicorn.so.
- Guest function (assembled with llvm-mc) sums an array in host memory (identity-mapped through `uc_mem_map_ptr`) and issues `svc #7`. The host hook reads x0, returns x0+1000, and the guest stores that to host memory.
- Result: `host_cb_sum=499500`, guest-written host cell `500500`. Correct, with zero copying.
- Numbers (CPU max, Release, 1 thread):
  - `uc_emu_start` round trip is **3.8 us**, including 2 register writes. This is the cost per host->guest call (JNI entry). JNI-chatty apps will feel it.
  - Steady-state loop is about **1.5 G guest insn/s**. This is a trivial `subs/b.ne` loop with block chaining; real code will be much slower.
- A native-bridge guest->host call is cheaper than 3.8 us if it stays inside one `uc_emu_start` (SVC hook, then resume).
  - In Unicorn 2.x a hook that stops emulation costs a restart, so don't stop for trampolines.

## Risks
1. QEMU 5.0 baseline: stale aarch64 feature set and TCG performance (see Evidence 1).
2. Softmmu TLB overhead on every access.
3. Host SIGSEGV from guest accesses to unmapped host pages, and coexistence with ART's handlers.
4. 3.8 us host->guest call cost.
5. Multi-engine memory coherence for atomics/TLB invalidation after `mprotect`/`munmap` (use `uc_ctl_flush_tlb`/`flush_tb` plus `remove_cache` on code writes).
6. Self-modifying code and `ic ivau` (JIT'd code in guest): need cache invalidation hooks.
7. Per-thread TCG buffers and warm-up memory.
8. GPL linking: libunicorn plus the bridge is a GPL combination, so ship it as its own .so (dlopen'd by the Apache-2 glue).
9. Bionic/AOSP clang build of Unicorn is untested.

## Ranked next steps
1. (1-2 d) Build Unicorn for x86_64 Android through a Soong `cc_library_shared` with an `Android.bp` in `translator/unicorn/` (or the CMake NDK route). Run the PoC as a bionic test binary on the x86_64 Android image.
2. (1 wk) Prove the real guest workload: load an arm64 bionic static or `libc.so` + `linker64` test binary via a minimal loader, with a syscall table (arm64 to x86_64 numbers/structs) in the SVC hook. Run `hello` and a small NDK `.so` through `dlopen`.
3. (3-5 d) Faults and threads: SIGSEGV -> guest fault via sigsetjmp, TPIDR_EL0/TLS, one engine per guest thread, `clone`, and atomics test (LDXR/STXR + LSE `ldadd`) across two threads.
4. (1-2 wk) Native-bridge shell: implement `NativeBridgeCallbacks` (initialize, loadLibrary, getTrampoline, JNI trampolines, `isSupported`) modeled on Berberis `native_bridge/`. Make the host->guest call fast: reuse the engine, no per-call setup.
5. (2-4 d, parallel) Benchmark the real workload (Dhrystone/CoreMark compiled for arm64) against qemu-user 10.0 and a dynarmic prototype. This is the go/no-go for keeping Unicorn or moving to dynarmic/linux-user.

Total to a first Android app running an arm64 `.so`: about 4-6 weeks.
