// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "proxy_ext.h"
namespace armtr_px {
// Host-call core: ints[6], fp bits[8], stk[16] -> raw result bits (float result in low 32 bits).
uint64_t raw_call(void* fn, char ret, const uint64_t* ints, const uint64_t* fp, const uint64_t* stk);
uint64_t norm(char c, uint64_t v);                 // normalise per sig char (int extension)
void store_ret(armtr_regs& r, char ret, uint64_t bits);
struct CurGuard { armtr_cpu* prev; explicit CurGuard(armtr_cpu* c); ~CurGuard(); };
}
