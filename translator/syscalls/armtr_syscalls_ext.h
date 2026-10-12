// SPDX-License-Identifier: GPL-2.0-or-later
// Additive extensions to include/armtr/syscalls.h (the contract header is unchanged).
#pragma once
#include <stdint.h>
#include "armtr/syscalls.h"
#ifdef __cplusplus
extern "C" {
#endif

// Returned by an intercept to let armtr_syscall handle the call normally.
#define ARMTR_SYSCALL_PASS INT64_MIN

// Offered every syscall BEFORE translation. Used by the embedder for things the in-process syscall
// layer cannot do alone: exit/exit_group (stop the guest thread instead of the host process),
// execve/execveat (load another aarch64 image), rt_sigreturn (restore a guest signal frame),
// rt_sigaction (after the default handling recorded it: query with armtr_syscalls_get_sigaction).
// Re-entrant calls of armtr_syscall made from inside the intercept are not intercepted again.
typedef int64_t (*armtr_syscall_intercept_fn)(armtr_cpu* cpu, uint64_t nr, const uint64_t args[6], void* user);
void armtr_syscalls_set_intercept(armtr_syscall_intercept_fn fn, void* user);

// Process-wide table of guest signal dispositions recorded by rt_sigaction (guest handlers are never
// installed in the host; the embedder delivers them). Layout = aarch64 kernel struct sigaction.
typedef struct { uint64_t handler, flags, restorer, mask; } armtr_guest_sigaction;
int armtr_syscalls_get_sigaction(int sig, armtr_guest_sigaction* out);   // 0 or -EINVAL

// Introspection: name of an implemented aarch64 syscall (NULL if unknown/unimplemented), and the
// x86_64 host number it is passed to (-1 if it has custom handling or none).
const char* armtr_syscall_name(uint64_t nr);
int armtr_syscall_host_nr(uint64_t nr);

#ifdef __cplusplus
}
#endif
