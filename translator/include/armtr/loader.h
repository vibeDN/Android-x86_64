// SPDX-License-Identifier: GPL-2.0-or-later
// Module contract: aarch64 ELF loader (shared objects and executables) into identity-mapped memory.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct armtr_image armtr_image;

// Resolves a symbol the loaded object imports but no loaded guest object defines (libc, liblog, ...).
// Return the guest address of a trampoline or guest function, or 0 if unknown.
typedef uint64_t (*armtr_resolver_fn)(const char* lib_hint, const char* symbol, void* user);

typedef struct {
  armtr_resolver_fn resolver;   // may be NULL
  void* resolver_user;
  const char* const* search_paths;   // directories for DT_NEEDED lookup, NULL terminated
} armtr_loader_opts;

// Maps PT_LOAD segments (mmap host memory at the chosen address, registers with the engine via
// armtr_mem_map), applies relocations (R_AARCH64_RELATIVE, GLOB_DAT, JUMP_SLOT, ABS64, TLS_*,
// IRELATIVE), loads DT_NEEDED recursively. Does NOT run constructors.
armtr_image* armtr_load(const char* path, const armtr_loader_opts* opts, char* err, size_t errlen);
uint64_t armtr_image_base(const armtr_image*);
uint64_t armtr_image_entry(const armtr_image*);                         // e_entry (executables)
uint64_t armtr_image_symbol(const armtr_image*, const char* name);     // 0 if missing
// DT_INIT / DT_INIT_ARRAY / DT_FINI_ARRAY addresses so the caller runs them on a CPU.
size_t   armtr_image_init_funcs(const armtr_image*, uint64_t* out, size_t max);
void     armtr_unload(armtr_image*);

#ifdef __cplusplus
}
#endif
