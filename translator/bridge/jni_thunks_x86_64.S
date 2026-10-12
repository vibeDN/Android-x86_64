// SPDX-License-Identifier: GPL-2.0-or-later
// Host-callable JNI trampolines (x86_64 SysV). ART calls a thunk as the native method; every thunk is
// `mov $index, %r11d; jmp armtr_jni_common` (16-byte slots, so thunk i = armtr_jni_thunks + 16*i).
// armtr_jni_common saves the SysV argument registers and calls
//   uint64_t armtr_jni_dispatch(uint32_t index, const uint64_t gpr[6], const uint64_t xmm[8], const uint64_t* stack)
// which marshals the call by shorty into AAPCS64 and runs the guest function (native_bridge.cc).
// The 64-bit result is returned in both %rax and %xmm0 (float results use its low 32 bits).
// No code is generated at runtime: the table is fixed (ARMTR_JNI_THUNKS entries).
#define ARMTR_JNI_THUNKS 16384

  .text
  .globl armtr_jni_thunks
  .hidden armtr_jni_thunks
  .type armtr_jni_thunks, @function
  .balign 16
armtr_jni_thunks:
  .set i, 0
  .rept ARMTR_JNI_THUNKS
  .balign 16
  movl $i, %r11d
  jmp armtr_jni_common
  .set i, i + 1
  .endr
  .size armtr_jni_thunks, . - armtr_jni_thunks

  .globl armtr_jni_common
  .hidden armtr_jni_common
  .type armtr_jni_common, @function
  .balign 16
armtr_jni_common:
  .cfi_startproc
  pushq %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset %rbp, -16
  movq %rsp, %rbp
  .cfi_def_cfa_register %rbp
  subq $112, %rsp                 // gpr[6] at -48(%rbp), xmm[8] at -112(%rbp); rsp stays 16-aligned
  movq %rdi, -48(%rbp)
  movq %rsi, -40(%rbp)
  movq %rdx, -32(%rbp)
  movq %rcx, -24(%rbp)
  movq %r8, -16(%rbp)
  movq %r9, -8(%rbp)
  movq %xmm0, -112(%rbp)
  movq %xmm1, -104(%rbp)
  movq %xmm2, -96(%rbp)
  movq %xmm3, -88(%rbp)
  movq %xmm4, -80(%rbp)
  movq %xmm5, -72(%rbp)
  movq %xmm6, -64(%rbp)
  movq %xmm7, -56(%rbp)
  movl %r11d, %edi
  leaq -48(%rbp), %rsi
  leaq -112(%rbp), %rdx
  leaq 16(%rbp), %rcx             // first stack argument of the original call
  call armtr_jni_dispatch
  movq %rax, %xmm0
  leave
  .cfi_def_cfa %rsp, 8
  ret
  .cfi_endproc
  .size armtr_jni_common, . - armtr_jni_common

  .section .note.GNU-stack, "", @progbits
