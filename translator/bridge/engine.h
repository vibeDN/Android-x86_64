/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Android-x86_64 project
 *
 * ENGINE interface: the only thing the NativeBridge glue needs from the
 * ARM64 CPU emulator (Unicorn / QEMU TCG / dynarmic / ...). Plain C so the
 * engine can live in its own GPL .so (libours_engine.so) and be dlopen'd.
 *
 * Contract:
 *  - Guest VA == host VA (identity mapping). The engine must be able to
 *    read/write any host-mapped user page directly; the bridge never copies.
 *  - One ours_engine_ctx per host thread (not thread-safe, but contexts are
 *    independent and share memory; atomics must be coherent across contexts).
 *  - Guest syscalls (SVC #0, x8 = nr, x0..x5 = args) and host trampolines
 *    (SVC #OURS_TRAMPOLINE_SVC_IMM, x16 = trampoline index) are delivered via
 *    callbacks, in the same ours_engine_run() call (no stop/restart).
 */
#ifndef OURS_NB_ENGINE_H_
#define OURS_NB_ENGINE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OURS_TRAMPOLINE_SVC_IMM 0x4e42

typedef struct ours_engine_ctx ours_engine_ctx;

/* Subset of AArch64 state visible to the bridge. */
typedef struct ours_cpu_regs {
  uint64_t x[31];   /* x0..x30 (x30 = lr) */
  uint64_t sp;
  uint64_t pc;
  uint64_t tpidr_el0;
  uint32_t nzcv, fpcr, fpsr;
  __uint128_t v[32];
} ours_cpu_regs;

/* Page protections for ours_engine_map_identity / protect. */
enum { OURS_PROT_R = 1, OURS_PROT_W = 2, OURS_PROT_X = 4 };

/* Fault reported by the engine while running guest code. */
typedef struct ours_fault {
  int kind;           /* 1 = unmapped read, 2 = unmapped write, 3 = fetch, 4 = prot, 5 = undefined insn, 6 = other */
  uint64_t pc, addr;
} ours_fault;

/* Callbacks installed once per process (before any context runs). */
typedef struct ours_engine_hooks {
  /* Linux syscall. Return value goes to x0 (negative errno convention as the kernel). */
  int64_t (*syscall)(ours_engine_ctx* ctx, uint64_t nr, const uint64_t args[6], void* user);
  /* Fault inside guest code; return 1 to resume at (possibly modified) regs, 0 to abort run with failure. */
  int (*fault)(ours_engine_ctx* ctx, const ours_fault* f, void* user);
  void* user;
} ours_engine_hooks;

/* Host function called from guest. Arguments are in ctx regs (x0..x7, v0..v7);
 * the callee writes the result to x0/v0. Engine resumes at x30 afterwards. */
typedef void (*ours_trampoline_fn)(ours_engine_ctx* ctx, void* callee, void* arg);

/* ---- process-wide ---- */
int ours_engine_init(const ours_engine_hooks* hooks);          /* 0 = ok */
const char* ours_engine_name(void);                            /* e.g. "unicorn-2.1.4" */

/* Identity mapping. Declares [addr, addr+len) of the *host* address space as
 * guest-visible with the given perms. Pages are already backed by the host
 * (bridge mmap'ed them). May be called once for the whole VA range and
 * re-called with new perms after mprotect/mmap/munmap (engine must then flush
 * TLB/TBs as needed). */
int ours_engine_map_identity(uint64_t addr, uint64_t len, int prot);
int ours_engine_unmap(uint64_t addr, uint64_t len);
int ours_engine_protect(uint64_t addr, uint64_t len, int prot);
void ours_engine_flush_icache(uint64_t addr, uint64_t len);     /* ic ivau, JIT, mprotect(+X) */

/* ---- per thread ---- */
ours_engine_ctx* ours_engine_create_context(void);
void ours_engine_destroy_context(ours_engine_ctx* ctx);
void ours_engine_get_regs(ours_engine_ctx* ctx, ours_cpu_regs* out);
void ours_engine_set_regs(ours_engine_ctx* ctx, const ours_cpu_regs* in);

/* Run guest function `entry` with up to 8 integer args (AAPCS64: x0..x7),
 * lr = a magic return address owned by the engine, until it returns.
 * Returns 0 and fills *ret (x0) on normal return; nonzero if aborted (fault
 * not resumed, ours_engine_request_stop, exit). Re-entrant: guest -> trampoline
 * -> host -> ours_engine_run() on the same ctx is allowed (nested calls, e.g. JNI callbacks). */
int ours_engine_run_guest_function(ours_engine_ctx* ctx, uint64_t entry,
                                   const uint64_t args[8], uint64_t* ret);
/* Same but continue from the current regs (used to start linker64 at its entry with a prepared stack). */
int ours_engine_run_from_regs(ours_engine_ctx* ctx);
void ours_engine_request_stop(ours_engine_ctx* ctx);            /* async-signal-safe: used by signal delivery */

/* ---- trampolines ---- */
/* Make guest address `guest_addr` (an interceptable stub or a freshly allocated
 * thunk) call `fn(ctx, callee, arg)` on the host. Engine may patch the stub
 * with `b thunk` or hook by address. Returns 0 on success. */
int ours_engine_register_host_trampoline(uint64_t guest_addr, ours_trampoline_fn fn, void* callee, void* arg);

#ifdef __cplusplus
}
#endif
#endif  // OURS_NB_ENGINE_H_
