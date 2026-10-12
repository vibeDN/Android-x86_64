# 04 - armtr-run (integration runner)

`translator/runner/armtr_run.cpp`: one host program linking engine + loader + syscalls (+ proxy sources, unused so far).
Build: `cmake -S translator/runner -B /mnt/storage-adata/tmp/run-build && make -C /mnt/storage-adata/tmp/run-build -j2`
(needs the patched Unicorn from `scripts/fetch-unicorn.sh`, default `translator/third_party-build/unicorn`).
Tests: `translator/runner/tests/run_tests.sh /mnt/storage-adata/tmp/run-build/armtr-run`.

    armtr-run [--sysroot DIR] [--host-env] [-E K=V] [--show-cpu] prog [args]     ARMTR_TRACE=1: log syscalls (futex with result)

## What it does (like execve)
* `armtr_load_raw` maps the exe (PIE at 0x5500000000) and, if PT_INTERP exists, the interpreter (0x7e00000000): segments only,
  no relocation, no DT_NEEDED. Static ET_EXEC works the same way. The guest linker64 then loads libc/libm/libdl itself.
* Stack (8 MiB, the main cpu's engine stack): strings, argc/argv/envp, auxv: AT_PHDR/PHENT/PHNUM, PAGESZ=4096, BASE (linker, 0
  for static), FLAGS, ENTRY, UID/EUID/GID/EGID, PLATFORM, HWCAP, CLKTCK, SECURE=0, RANDOM, HWCAP2, EXECFN, MINSIGSTKSZ.
  No AT_SYSINFO_EHDR: bionic skips the vDSO and uses the clock_gettime syscall (verified, stress/hello run).
  Default env: ANDROID_ROOT, ANDROID_DATA, PATH (`--host-env` passes the host's).
* HWCAP/HWCAP2 are derived by running `mrs` of ID_AA64ISAR0/ISAR1/PFR0/MMFR2 on the engine's CPU model (UC_CPU_ARM64_MAX) in the
  guest, so they match what Unicorn implements (`--show-cpu`). Result: FP ASIMD AES PMULL SHA1 SHA2 CRC32 ATOMICS FPHP ASIMDHP CPUID
  ASIMDRDM JSCVT FCMA LRCPC DCPOP SHA3 SM3 SM4 ASIMDDP SHA512 ASIMDFHM DIT USCAT ILRCPC FLAGM, HWCAP2 DCPODP FLAGM2 FRINT
  (= ARMv8.4-ish). SVE, PAC, MTE, BTI, RNG, BF16, I8MM are never advertised (bionic would use tagging/PAC prctls we do not emulate).
* Syscall intercept (`armtr_syscalls_set_intercept`): exit_group -> `_exit(code)` (guest fds/memory are the host's, nothing to
  flush); exit -> stop this guest thread (main waits for the others); set_tid_address emulated (host libc owns the real one);
  path mapper for openat/faccessat/faccessat2/newfstatat/statx/statfs/readlinkat: absolute paths under /system /apex
  /linkerconfig /vendor /product /system_ext /odm /data/local/tmp get `--sysroot` prepended (also `ARMTR_SYSROOT`);
  readlinkat("/proc/self/exe") returns the guest exe. Everything else passes to the syscalls module.
* Threads (`armtr_syscalls_set_thread_hook`): detached host pthread + own armtr_cpu; child writes its host tid to
  PARENT_SETTID/CHILD_SETTID, parent waits for it; on exit CLEARTID is emulated (write 0 safely, FUTEX_WAKE shared+private).
* A guest fault prints pc/lr/sp, the mapped file + offset of pc and exits 139.

## Not done
rt_sigreturn / guest signal delivery (handlers are only recorded by the syscalls module; a guest SIGSEGV is an armtr fault),
fork/exec of guest images, proxy/JNIEnv wiring (jni_host_dyn uses a fake VM entirely inside the guest, no host proxies needed).

## Cross-module changes made for this
* loader: `armtr_load_raw()` (additive, `armtr_loader_ext.h`) - map-only load, optional fixed base.
* engine: top-byte-ignore. Android arm64 userspace relies on TBI (bionic's scudo ORs 0x02<<56 into every heap pointer).
  `engine.cc` now selects Unicorn's virtual TLB (`UC_TLB_VIRTUAL`) with a `UC_HOOK_TLB_FILL` that strips bits 63:56.
* Unicorn (scripts/fetch-unicorn.sh): `atomic_template.h` xchg helper (SWP/SWPAL, bionic's mutex) was a non-atomic load+store;
  now `atomic_xchg__nocheck`. Without it pthread_test_dyn deadlocks. Rebuild Unicorn after re-running the script.
