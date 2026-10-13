// SPDX-License-Identifier: GPL-2.0-or-later
// Touches Unicorn 2.1.4 internals (uc_priv.h is C-only): fix bookkeeping after we siglongjmp'ed out of
// uc_emu_start because of a host SIGSEGV in translated code.
#include <unicorn/unicorn.h>
#include "uc_priv.h"

int armtr_uc_nested_level(uc_engine* uc) { return uc->nested_level; }

void armtr_uc_repair(uc_engine* uc, int nested_level) {
  uc->nested_level = nested_level;
  uc->stop_request = false;
  uc->quit_request = false;
  if (nested_level == 0) uc->emulation_done = true;
}

// Per-engine performance setup (fields added by scripts/fetch-unicorn.sh, docs/translator/06-performance.md):
//  * no_exit_request: Unicorn emits a helper call (check_exit_request) after every guest load/store so a hook's
//    uc_emu_stop takes effect at once. We never need that: unmapped accesses siglongjmp out, kicks are seen at
//    the next TB start.
//  * armtr_direct_mem: loads/stores go straight to host memory (guest VA == host VA), no softmmu TLB.
//  * armtr_ic_ivau: called for IC IVAU (direct stores bypass Unicorn's self-modifying-code tracking).
void armtr_uc_tune(uc_engine* uc, int direct, void (*ic_ivau)(uc_engine*, uint64_t)) {
  uc->no_exit_request = true;
  uc->armtr_direct_mem = direct;
  uc->armtr_ic_ivau = ic_ivau;
}

// Clears a pending uc_emu_stop() (armtr_kick handled inside a hook: no need to leave uc_emu_start).
void armtr_uc_unstop(uc_engine* uc) { uc->stop_request = false; }

// Sets the guest pc (and the TB-local state) from a host pc inside translated code; 0 if host_pc is not in a TB.
// host_pc is a return address (call into a helper) or a faulting insn + GETPC_ADJ (2 on x86_64).
bool cpu_restore_state_aarch64(struct CPUState* cpu, uintptr_t host_pc, bool will_exit);
int armtr_uc_restore(uc_engine* uc, uintptr_t host_pc) { return cpu_restore_state_aarch64(uc->cpu, host_pc, true); }
