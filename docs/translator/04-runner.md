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
* Guest signals (`runtime/guest_rt.cpp` + engine hook, see below). A guest fault with no guest handler (SIG_DFL/IGN,
  blocked, or only bionic's linker-installed debuggerd handler) prints pc/lr/sp, the mapped file + offset of pc and exits 139.

## On the device: /system/bin/armtr-run + binfmt_misc (B3)
* Soong `cc_binary` `armtr-run` (`translator/runner/Android.bp`, static engine/loader/syscalls/runtime/hostlibs/proxy/
  unicorn). Guest files are the bridge's: PT_INTERP `/system/bin/linker64` is loaded from `/system/bin/armtr/linker64`
  when that exists (also under `--sysroot`), the guest linker then uses `/system/etc/ld.config.arm64.txt`
  (`/system`, `/data` -> `/system/lib64/armtr`). Guest proxy libs (liblog, libz, ...) resolve through hostlibs
  (syscalls 0x4153/0x4154, like the bridge).
* Exec: kernel binfmt_misc (`CONFIG_BINFMT_MISC=y` already). `armtr-run.rc` (installed via `init_rc`) mounts it on
  early-init and registers `/system/etc/binfmt_misc/arm64_exe` / `arm64_dyn` (aarch64 ET_EXEC/ET_DYN, OSABI ignored)
  with interpreter `/system/bin/armtr-run`, flags `POF`. This is the Berberis mechanism (`berberis.rc`), same switch
  `ro.enable.native.bridge.exec=1` (set in `ours_cf_x86_64_phone.mk`). armtr-run detects binfmt mode by `AT_EXECFD`
  (O flag): argv = `armtr-run <path> <argv0> <args>`, the host environment is passed unchanged. So app
  ProcessBuilder/Runtime.exec, `sh -c`, and guest execve of arm64 files all go through the kernel; nothing in the
  syscalls module (execve passes through).
* SELinux: not added. The image boots `androidboot.selinux=permissive`. Enforcing would need
  `allow init binfmt_miscfs:file w_file_perms` and `execmem` for `shell` (apps have execmem already).
* Fixes found on the way (runtime): fork children reset the guest thread count (a Go child that fails exec ends with
  the plain `exit` syscall and armtr-run waited for the parent's threads forever); `/proc/self/stat` opened by the
  guest reports field 28 (startstack) = the guest initial sp, so bionic's main-thread `pthread_getattr_np` finds the
  guest stack (Go cgo aborted with "bad stack bounds"). Test: `runner/tests/proc_test.c` (M4-proc).

## Guest signals
* Async (kill/tgkill/raise, SIGALRM, SIGCHLD, SIGPIPE, ...): a guest handler gets a host handler (`SA_SIGINFO|SA_ONSTACK`, all
  signals masked, never `SA_RESTART`) unless the host already handles that signal (ART/debuggerd in an app keep theirs).
  It stores the siginfo per guest thread (threads without a cpu: process-wide queue, taken by the next guest thread passing a
  syscall) and calls `armtr_kick` (flag + `uc_emu_stop`). The engine calls the runtime's signal hook right after the current
  syscall (svc hook, without leaving `uc_emu_start`) or when `uc_emu_start` returns. SIG_DFL/SIG_IGN are host dispositions.
* Frame = arm64 kernel layout (arch/arm64/kernel/signal.c): `rt_sigframe{siginfo(128), ucontext}` (4688 bytes), mcontext at
  +304, `__reserved` at +592 with `fpsimd_context` (magic 0x46508001, size 528), `esr_context` for SIGSEGV/SIGBUS (data/insn
  abort ESR, WnR from the host page-fault error code), zero end marker; `{x29,x30}` frame record above it; x0=sig x1=&info
  x2=&uc x29=&record x30=sa_restorer (else a built-in `mov x8,#139; svc #0`). sa_mask/SA_NODEFER/SA_RESETHAND/SA_ONSTACK
  (sigaltstack from the syscalls module) honoured; the guest mask is the host thread mask (rt_sigprocmask passes through).
  SA_RESTART: a syscall interrupted with -EINTR is restarted (pc-4, original x0) for read/write/readv/writev/ioctl/openat/
  fcntl/flock/wait4/waitid/accept/accept4/connect/send*/recv*; others return -EINTR (as nanosleep/poll/epoll/futex do).
* `rt_sigreturn` (intercept 139) restores x0-x30, sp, pc, NZCV, fpsr/fpcr/v0-v31 and the mask from the frame.
* Sync: Unicorn-unmapped accesses (the unmapped hook siglongjmps out at once, regs exact), host SIGSEGV/SIGBUS in translated
  code (guest pc recovered with `cpu_restore_state` from the host pc, or from the helper's return address found on the
  abandoned host stack; si_code from the host), UDF -> SIGILL/ILL_ILLOPC, BRK -> SIGTRAP/TRAP_BRKPT, all with si_addr/pc exact.
* Tests: `runner/tests/sig_test.c` (built by run_tests.sh, cases M3-*): siginfo/mask, NODEFER/RESETHAND, all v-regs + temps +
  NZCV across a clobbering handler, SIGALRM during a syscall-free loop, a 500 us timer storm over FP code, SA_RESTART vs EINTR
  for a blocked read in another thread, four SIGSEGV kinds (null, munmapped, PROT_NONE, TLB-hot mprotect) with exact
  pc/si_addr/esr and skip-the-insn resume, SIGILL, SIGTRAP, stack overflow -> SA_ONSTACK handler -> siglongjmp, SIGPIPE
  handler/ignored, SIGCHLD from fork, SIG_DFL kill (rc 138), unhandled SIGSEGV (rc 139). Engine: test_engine signal-hook case.

## Not done
Signals: RT signals are not queued (one pending instance per signal, like standard ones); sigreturn does not restore
uc_stack (sigaltstack); no extra_context/SVE records; a kick landing in `uc_emu_start`'s prologue waits for the next syscall;
SS_ONSTACK is not reported by sigaltstack(); bionic's debuggerd handlers in linker64 are never run.
exec of arm64 images on a host without our binfmt_misc registration (the host test machine's qemu-aarch64 entry
takes them), proxy/JNIEnv wiring (jni_host_dyn uses a fake VM entirely inside the guest, no host proxies needed).

## Cross-module changes made for this
* loader: `armtr_load_raw()` (additive, `armtr_loader_ext.h`) - map-only load, optional fixed base.
* engine: top-byte-ignore. Android arm64 userspace relies on TBI (bionic's scudo ORs 0x02<<56 into every heap pointer).
  `engine.cc` now selects Unicorn's virtual TLB (`UC_TLB_VIRTUAL`) with a `UC_HOOK_TLB_FILL` that strips bits 63:56.
  Since 06-performance.md, data accesses bypass the TLB and the translator strips the tag (TCR TBI bits).
* Unicorn (scripts/fetch-unicorn.sh): `atomic_template.h` xchg helper (SWP/SWPAL, bionic's mutex) was a non-atomic load+store;
  now `atomic_xchg__nocheck`. Without it pthread_test_dyn deadlocks. Rebuild Unicorn after re-running the script.
