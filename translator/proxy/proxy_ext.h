// SPDX-License-Identifier: GPL-2.0-or-later
// Additive extensions to armtr/proxy.h (proxy.h itself is unchanged).
//
// Signature strings: "<ret>(<args>)" or "<ret><args>" ("(" ")" optional); an optional trailing "..."
// marks a variadic function. Chars (arg and ret):
//   v void (ret only)  i int32 (sign-extended)  u uint32  l int64/size_t  p pointer
//   f float  d double  z bool(uint8)  b int8  h int16  c uint16 (jchar)
// AAPCS64 -> SysV mapping: ints/ptrs x0-x7 then stack; fp v0-v7 then stack; each stack arg is an 8-byte
// slot at sp. Int and FP overflow share ONE guest stack in argument order. Host side: 6 int regs, 8 xmm,
// overflow to host stack in argument order. Max 24 named args.
// Variadic ("..."): the unnamed args are forwarded as the remaining x regs, all remaining v regs (as
// doubles) and 8 guest stack slots, so a host variadic fn (e.g. snprintf) sees the same ints/doubles.
// Ceiling: >8 unnamed FP args, or FP spilled to the stack interleaved with ints, are not reordered.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <armtr/proxy.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char* name; const char* sig; void* fn; } armtr_proxy_def;
#define ARMTR_PROXY_DEF(name, sig, fn) { #name, sig, (void*)(fn) }
// ARMTR_PROXY(open, "i(pii)", host_open) registers one proxy now; returns the guest address (0 on error).
#define ARMTR_PROXY(name, sig, fn) armtr_proxy_make(#name, sig, (void*)(fn))
int armtr_proxy_make_table(const armtr_proxy_def* defs, int n);   // returns number registered OK

// Bare stub (identity-mapped, 8-byte slot, host-writable, `ret`); optional raw trampoline instead of a sig adapter.
uint64_t armtr_proxy_stub_alloc(void);
uint64_t armtr_proxy_make_raw(const char* name, armtr_trampoline_fn fn, void* user);
// Guest-visible (registered with armtr_mem_map), RW, zeroed, 16-aligned data; never freed.
void* armtr_proxy_data_alloc(size_t size);

// Configure how the thread-local CPU is created for armtr_proxy_call_guest. `init` (optional) runs on
// every new CPU (e.g. set tpidr_el0). The guest stack is allocated by the proxy (1 MiB per thread).
void armtr_proxy_config(armtr_svc_fn svc, void* svc_user, void (*init)(armtr_cpu*, void*), void* init_user);
// CPU of the innermost running trampoline on this thread, else the thread-local CPU (NULL if none yet).
armtr_cpu* armtr_proxy_current_cpu(void);

// ---- AAPCS64 variadic access ------------------------------------------------------------------------
// Reader over either (a) the register file+stack of a variadic call or (b) a guest va_list struct
// {void* stack; void* gr_top; void* vr_top; int gr_offs, vr_offs;} (bionic arm64 passes va_list by
// pointer to that 32-byte struct; the callee gets a pointer in the next x reg). Differences vs x86_64
// SysV va_list: different layout/offset math, FP slots are 16 bytes in the save area, and the host
// va_list cannot be passed through: use armtr_va_to_host_* to rebuild one.
typedef struct armtr_va {
  const armtr_regs* regs;               // register mode if non-NULL
  unsigned gr, vr;                      // next x / v register (register mode)
  const uint64_t* stack;                // next stack slot
  const char *gr_top, *vr_top;          // list mode
  int32_t gr_offs, vr_offs;
} armtr_va;
// `named_int`/`named_fp` = number of x / v regs consumed by named args (stack slots are taken from sp).
void armtr_va_from_regs(armtr_va*, const armtr_regs*, unsigned named_int, unsigned named_fp);
void armtr_va_from_list(armtr_va*, uint64_t guest_va_list_ptr);
uint64_t armtr_va_int(armtr_va*);       // next int/pointer slot (full 64 bits; truncate for int)
double   armtr_va_double(armtr_va*);    // next double (floats are promoted to double in varargs)

// Host (x86_64 SysV) va_list rebuilt from guest args. Do not copy the struct after init.
typedef struct { uint64_t slot[32]; va_list ap; } armtr_host_va;
// types: i u l p d (f treated as d). Returns 0, or -1 (unsupported arch / too many).
int armtr_va_to_host_types(armtr_va* g, const char* types, armtr_host_va* out);
// Derives the types from a printf-style format (%d %s %f %*d %ld %zu %p ...). `L` (long double) -> -1.
int armtr_va_to_host_printf(armtr_va* g, const char* fmt, armtr_host_va* out);

// ---- JNI --------------------------------------------------------------------------------------------
// Index of a JNINativeInterface slot by name (e.g. "CallIntMethodA"), -1 if unknown. 233 slots total.
int armtr_proxy_jni_index(const char* name);
// jmethodIDs seen through the proxied GetMethodID/GetStaticMethodID are cached with their descriptor
// automatically; use this for ids obtained otherwise (FromReflectedMethod, host-created ids).
void armtr_proxy_jni_register_method(void* jmethodID, const char* descriptor);

#ifdef __cplusplus
}
#endif
