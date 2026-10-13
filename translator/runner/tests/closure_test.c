// SPDX-License-Identifier: GPL-2.0-or-later
// Test: runtime code generation via mmap/mprotect (libffi closure pattern).
// Bug B8: translator's instruction cache must be invalidated when code becomes executable.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "armtr/engine.h"

// Closure-like function: returns x0 + 42
static const uint32_t closure_code[] = {
  0xd2800540,  // mov x0, #42
  0xd65f03c0   // ret
};

int test_closure_mprotect(void) {
  // Allocate RW memory
  uint64_t* closure = (uint64_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (closure == MAP_FAILED) return 1;

  // Write closure code
  memcpy(closure, closure_code, sizeof(closure_code));

  // Make it executable (this should trigger cache invalidation in the bridge)
  if (mprotect(closure, 4096, PROT_READ | PROT_EXEC) != 0) return 1;

  // Execute the closure
  armtr_cpu* cpu = armtr_cpu_create(NULL, NULL);
  if (!cpu) return 1;

  uint64_t ret;
  int err = armtr_call(cpu, (uint64_t)closure, NULL, 0, &ret);
  armtr_cpu_destroy(cpu);

  munmap(closure, 4096);

  // Verify result
  return (err != 0 || ret != 42) ? 1 : 0;
}

int test_closure_mmap_exec(void) {
  // Alternative: allocate RWX memory directly (also exercises the code path)
  uint64_t* closure = (uint64_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (closure == MAP_FAILED) return 1;

  memcpy(closure, closure_code, sizeof(closure_code));

  armtr_cpu* cpu = armtr_cpu_create(NULL, NULL);
  if (!cpu) return 1;

  uint64_t ret;
  int err = armtr_call(cpu, (uint64_t)closure, NULL, 0, &ret);
  armtr_cpu_destroy(cpu);

  munmap(closure, 4096);

  return (err != 0 || ret != 42) ? 1 : 0;
}
