// SPDX-License-Identifier: GPL-2.0-or-later
// Fake armtr engine for proxy tests: implements engine.h without any CPU emulation.
#pragma once
#include <functional>
#include <armtr/engine.h>
struct armtr_cpu { armtr_regs r; };
// Simulates the guest executing the registered trampoline at `addr` on `cpu`.
void fake_tramp(armtr_cpu* cpu, uint64_t addr);
// Registers a "guest function": armtr_call(entry) runs fn(cpu) after setting x0.. and pc.
void fake_guest_fn(uint64_t entry, std::function<void(armtr_cpu*)> fn);
bool fake_mapped(uint64_t addr, int prot);
int fake_cpus_created();
int fake_cpus_destroyed();
