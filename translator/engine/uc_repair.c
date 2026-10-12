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
