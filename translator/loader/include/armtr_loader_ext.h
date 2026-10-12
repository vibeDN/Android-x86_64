// SPDX-License-Identifier: GPL-2.0-or-later
// Additive extensions to armtr/loader.h (the base contract is unchanged).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "armtr/loader.h"
#ifdef __cplusplus
extern "C" {
#endif

// --- global knobs (call before armtr_load) ---
// Default 0 (strict): a non-weak import that neither a loaded guest object nor the resolver provides
// fails the load, the name is in err. Lenient (1): the slot is set to 0, the import is recorded and
// listed by armtr_image_unresolved*. Weak undefined imports are never errors.
void armtr_loader_set_lenient(int on);
// Size of the thread control block placed at tpidr_el0 before the static TLS blocks (aarch64 TLS
// variant 1). Default 16 (the ABI minimum). Used for TPREL/TLSDESC offsets. Set before loading.
void armtr_loader_set_tcb_size(size_t bytes);

// --- TLS ---
typedef struct {
  uint64_t init_image;      // guest address of the .tdata initialisation image
  uint64_t filesz, memsz;   // copy filesz bytes, zero the rest up to memsz
  uint64_t align;
  uint32_t module_id;       // value written for R_AARCH64_TLS_DTPMOD64 (1-based, global, never reused)
  uint64_t static_offset;   // offset of this block from tpidr_el0 (>= tcb size); TPREL64/TLSDESC use it
} armtr_tls_info;
// 0 and *out filled if the image has a PT_TLS segment, -1 otherwise.
int    armtr_image_tls(const armtr_image*, armtr_tls_info* out);
// Every PT_TLS image gets a static offset at load time. This is tcb size + all static blocks so far:
// the guest libc allocates at least this many bytes for a thread's TCB+static TLS.
size_t armtr_tls_static_size(void);
// TLSDESC: implemented for the static TLS model: desc[0] = guest stub `ldr x0,[x0,#8]; ret`
// (in a loader-owned RX page registered with armtr_mem_map), desc[1] = offset from tpidr_el0.

// --- IRELATIVE and STT_GNU_IFUNC-symbol relocations: need a guest call ---
// Call the guest function at `resolver` (no args), then pass its return value to _set which stores it.
size_t armtr_image_irelative_count(const armtr_image*);
int    armtr_image_irelative(const armtr_image*, size_t i, uint64_t* slot, uint64_t* resolver);
int    armtr_image_irelative_set(const armtr_image*, size_t i, uint64_t value);   // 0 ok

// --- init/fini ---
// armtr_image_init_funcs (base API) returns the number of functions needed (may exceed max; at most
// max are written) in execution order: for each image dependencies first; per image
// DT_PREINIT_ARRAY, DT_INIT, DT_INIT_ARRAY. Images already marked done are skipped.
size_t armtr_image_fini_funcs(const armtr_image*, uint64_t* out, size_t max);  // reverse order: FINI_ARRAY reversed, then DT_FINI; image before its deps
void   armtr_image_init_done(armtr_image*);   // marks image and its dependency tree as initialised

// --- raw mapping (what the kernel does for execve) ---
// Maps PT_LOAD segments only: no PT_DYNAMIC needed (static executables work), no DT_NEEDED, no
// relocations, no TLS allocation, RELRO left writable. Registered with the engine. `base` (0 = any) is the
// load address of the lowest PT_LOAD for ET_DYN, must be aligned to the largest p_align; ET_EXEC ignores it.
// Use for the guest ELF interpreter and the main executable when the guest linker does the rest. The
// result supports armtr_image_{base,entry,phdr,path}; do not pass it to the symbol/init APIs.
armtr_image* armtr_load_raw(const char* path, uint64_t base, char* err, size_t errlen);

// --- introspection ---
const char* armtr_image_path(const armtr_image*);
const char* armtr_image_soname(const armtr_image*);                // "" if none
size_t      armtr_image_size(const armtr_image*);                  // mapped span in bytes
size_t      armtr_image_dep_count(const armtr_image*);             // DT_NEEDED found on disk and loaded
const armtr_image* armtr_image_dep(const armtr_image*, size_t i);
size_t      armtr_image_missing_count(const armtr_image*);         // DT_NEEDED not found in search paths (provided by the resolver)
const char* armtr_image_missing(const armtr_image*, size_t i);
size_t      armtr_image_unresolved_count(const armtr_image*);      // imports of THIS image nobody provided (deduplicated)
const char* armtr_image_unresolved(const armtr_image*, size_t i);
size_t      armtr_image_reloc_count(const armtr_image*);           // relocations applied
// For AT_PHDR/AT_PHNUM of an executable.
int armtr_image_phdr(const armtr_image*, uint64_t* addr, uint64_t* num, uint64_t* entsize);

#ifdef __cplusplus
}
#endif
