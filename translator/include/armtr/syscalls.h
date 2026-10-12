// SPDX-License-Identifier: GPL-2.0-or-later
// Module contract: aarch64 Linux syscall translation to the x86_64 host kernel.
#pragma once
#include <stdint.h>
#include "engine.h"
#ifdef __cplusplus
extern "C" {
#endif

// Matches armtr_svc_fn. nr is the aarch64 syscall number. Translates number and argument layouts
// (struct stat, sigaction, flags like O_*, clone, mmap, ...) and executes it on the host.
// Returns the aarch64-style result (negative errno on failure, as the kernel ABI).
// Must handle: read/write/openat/close/fstat/newfstatat/lseek/mmap/munmap/mprotect/mremap/madvise,
// futex, clock_gettime/nanosleep, getpid/gettid/tgkill, exit/exit_group, rt_sigaction/sigprocmask,
// prctl, ioctl (common), socket family, epoll/eventfd/pipe2, set_tid_address, getrandom, clone
// (thread creation hook below), brk (not needed on bionic), sched_*, membarrier, ... .
int64_t armtr_syscall(armtr_cpu* cpu, uint64_t nr, const uint64_t args[6], void* user);

// Thread creation: the syscalls module calls this for clone(CLONE_VM|CLONE_THREAD...). The embedder
// starts a host thread with its own armtr_cpu running the child at the given regs.
typedef int (*armtr_thread_start_fn)(const armtr_regs* child_regs, uint64_t flags, uint64_t child_tid_ptr,
                                     uint64_t parent_tid_ptr, void* user);
void armtr_syscalls_set_thread_hook(armtr_thread_start_fn fn, void* user);

#ifdef __cplusplus
}
#endif
