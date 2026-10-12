// SPDX-License-Identifier: GPL-2.0-or-later
// armtr: ARM64 -> x86_64 in-process translator. Module contract: CPU engine.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Memory model: guest virtual address == host virtual address (identity map). Guest code reads and
// writes the same memory as the host process (JNI pointers, Java arrays need no copying).
// The engine only has to make host regions executable/readable for the guest: armtr_mem_map()
// registers a host range (already mmap-ed by the loader/guest mmap) with the CPU core.

typedef struct armtr_cpu armtr_cpu;   // one per guest thread

typedef struct {
  uint64_t x[31];   // x0..x30
  uint64_t sp;
  uint64_t pc;
  uint64_t tpidr_el0;   // guest TLS pointer
  uint64_t nzcv;
  uint8_t  v[32][16];   // NEON/FP registers
  uint32_t fpcr, fpsr;
} armtr_regs;

// Called when guest executes `svc #0`: syscall number in x8, args x0..x5; returns value for x0.
// Implemented by the syscalls module.
typedef int64_t (*armtr_svc_fn)(armtr_cpu* cpu, uint64_t nr, const uint64_t args[6], void* user);
// Called when guest jumps to a registered host-trampoline address (see proxy.h).
typedef void (*armtr_trampoline_fn)(armtr_cpu* cpu, void* user);

int  armtr_init(void);                                        // 0 on success
armtr_cpu* armtr_cpu_create(armtr_svc_fn svc, void* svc_user);
void armtr_cpu_destroy(armtr_cpu*);
int  armtr_mem_map(uint64_t addr, size_t len, int prot);      // prot: bit0 R, bit1 W, bit2 X
int  armtr_mem_unmap(uint64_t addr, size_t len);
int  armtr_mem_protect(uint64_t addr, size_t len, int prot);
void armtr_regs_get(armtr_cpu*, armtr_regs*);
void armtr_regs_set(armtr_cpu*, const armtr_regs*);
// Registers a guest address that, when executed, calls `fn` on the host and then returns to x30.
int  armtr_trampoline_add(uint64_t guest_addr, armtr_trampoline_fn fn, void* user);
// Runs the guest from regs.pc until it returns to `stop_addr` (set as x30 by the caller) or exits.
// Returns 0 normally, <0 on engine error / unhandled guest fault (see armtr_last_error()).
int  armtr_run(armtr_cpu*, uint64_t stop_addr);
const char* armtr_last_error(armtr_cpu*);

// Convenience: call guest function `entry` with up to 8 integer args; result in *ret (x0).
int  armtr_call(armtr_cpu*, uint64_t entry, const uint64_t* args, int nargs, uint64_t* ret);


// ---- Additive API (engine module) ----
// Like armtr_cpu_create but with an explicit guest stack size (0 = default 1 MiB).
armtr_cpu* armtr_cpu_create_ex(armtr_svc_fn svc, void* svc_user, size_t stack_size);
// Requests the running armtr_run()/armtr_call() of this cpu to return (e.g. from an exit syscall).
void armtr_stop(armtr_cpu*);
// Guest stack of this cpu (lowest usable address, usable size, initial sp = base+size).
void armtr_cpu_stack(armtr_cpu*, uint64_t* base, size_t* size);
// Details of the last fault: kind 0 none, 1 read, 2 write, 3 fetch, 4 other; addr/pc may be NULL.
// pc is exact for unmapped faults, approximate (start of the translation block) for host SIGSEGV faults.
int  armtr_fault_info(armtr_cpu*, uint64_t* addr, uint64_t* pc);

#ifdef __cplusplus
}
#endif
