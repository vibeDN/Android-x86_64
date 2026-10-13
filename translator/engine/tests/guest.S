// SPDX-License-Identifier: GPL-2.0-or-later
// Hand-written aarch64 test functions; all offsets are listed in tests/guest_offsets.h order (see below).
// Layout: each function starts at a fixed 0x40-byte slot: slot N at N*0x40.
    .text
    .balign 64
// slot 0: x0 = a + b*2  (add x0, x0, x1, lsl 1)
f_add:
    add x0, x0, x1, lsl #1
    ret
    .balign 64
// slot 1: sum8(x0..x7) -> x0
f_sum8:
    add x0, x0, x1
    add x0, x0, x2
    add x0, x0, x3
    add x0, x0, x4
    add x0, x0, x5
    add x0, x0, x6
    add x0, x0, x7
    ret
    .balign 64
// slot 2: svc round trip: x0 = syscall(nr=x0, arg0=x1); returns result
f_svc:
    mov x8, x0
    mov x0, x1
    svc #0
    ret
    .balign 64
// slot 3: call trampoline at address x0 with args (x1,x2) and return ret+1000, preserving lr
f_tramp:
    stp x29, x30, [sp, #-16]!
    mov x16, x0
    mov x0, x1
    mov x1, x2
    blr x16
    add x0, x0, #1000
    ldp x29, x30, [sp], #16
    ret
    .balign 64
// slot 4: NEON: v0 = x0 ptr(16 bytes) + x1 ptr(16 bytes) as u32x4 add, store to x2; returns faddp sum of v2 floats in s0
f_neon:
    ld1 {v0.4s}, [x0]
    ld1 {v1.4s}, [x1]
    add v2.4s, v0.4s, v1.4s
    st1 {v2.4s}, [x2]
    ret
    .balign 64
// slot 5: sum u64 array: x0 = ptr, x1 = n; stores total to [x2]; returns total
f_sumarr:
    mov x3, #0
1:  cbz x1, 2f
    ldr x4, [x0], #8
    add x3, x3, x4
    sub x1, x1, #1
    b 1b
2:  str x3, [x2]
    mov x0, x3
    ret
    .balign 64
// slot 6: load from [x0] (fault test)
f_load:
    ldr x0, [x0]
    ret
    .balign 64
// slot 7: store x1 to [x0]
f_store:
    str x1, [x0]
    ret
    .balign 64
// slot 8: spin x0 iterations (2 insns each)
f_spin:
1:  subs x0, x0, #1
    b.ne 1b
    ret
    .balign 64
// slot 9: increment counter with ldxr/stxr x1 times at [x0]; atomic add via ldadd also
f_atomic:
1:  ldaxr x3, [x0]
    add x3, x3, #1
    stlxr w4, x3, [x0]
    cbnz w4, 1b
    sub x1, x1, #1
    cbnz x1, 1b
    ret
    .balign 64
// slot 10: tpidr_el0 read -> x0 ; sets x0=tpidr
f_tls:
    mrs x0, tpidr_el0
    ret
    .balign 64
// slot 11: float: s0*2 via fmul d0,d0,d0 ; returns in d0 ; also sets nzcv by cmp
f_fp:
    fmul d0, d0, d0
    ret
    .balign 64
// slot 12: calls trampoline x0 that re-enters guest (nested); returns x0
f_nest:
    stp x29, x30, [sp, #-16]!
    mov x16, x0
    mov x0, x1
    blr x16
    ldp x29, x30, [sp], #16
    ret
    .balign 64
// slot 13: store x1 to [x0] as new code and publish it like __clear_cache: dc cvau; dsb; ic ivau; dsb; isb
f_store_code:
    str x1, [x0]
    dc cvau, x0
    dsb ish
    ic ivau, x0
    dsb ish
    isb
    ret
