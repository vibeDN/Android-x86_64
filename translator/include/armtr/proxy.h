// SPDX-License-Identifier: GPL-2.0-or-later
// Module contract: guest -> host calls (proxy libraries, JNIEnv/JavaVM tables) and host -> guest calls.
#pragma once
#include <stdint.h>
#include "engine.h"
#ifdef __cplusplus
extern "C" {
#endif

// Allocates a guest-visible stub address for host function `fn` and registers it with the engine
// (armtr_trampoline_add). The stub reads AAPCS64 args (x0-x7, v0-v7) from the CPU, calls the host
// function with SysV x86_64 convention, writes the result to x0 / v0.
// `sig` is a short signature string, e.g. "i(pi)" = int f(void*, int); chars:
//   v void, i int32, l int64/size_t, p pointer, f float, d double, z bool.
uint64_t armtr_proxy_make(const char* name, const char* sig, void* host_fn);
// Lookup of a previously made proxy (used by the loader resolver).
uint64_t armtr_proxy_lookup(const char* name);
// Builds a guest-visible JNIEnv* / JavaVM* wrapping the real host ones: each function-table entry is a
// proxy stub that forwards to the host table (with the right signature per entry).
uint64_t armtr_proxy_jnienv(void* host_jnienv);
uint64_t armtr_proxy_javavm(void* host_javavm);
// Calls a guest function from the host with the host's thread-local CPU (creates it on first use).
int armtr_proxy_call_guest(uint64_t entry, const uint64_t* args, int nargs, uint64_t* ret);

#ifdef __cplusplus
}
#endif
