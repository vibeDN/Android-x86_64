// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
struct FakeRegion { uint64_t addr; size_t len; int prot; };
size_t fake_region_count();
bool fake_region_find(uint64_t addr, FakeRegion* out);
