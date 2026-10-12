#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates the guest-side (aarch64) proxy libraries of the host platform libraries the guest Bionic lacks
(liblog, libz, libandroid, libnativewindow, libjnigraphics, libEGL, libGLESv1_CM, libGLESv2/3, libOpenSLES).

Each is a real arm64 .so with the platform library's soname that the guest linker loads like any library.
It exports every function of that library in translator/hostlibs/gen/hostlibs_tables.inc as a stub:

    sym:  adrp x16, desc ; add x16, x16, :lo12:desc   // desc = {target, "lib", "sym"}
          ldr  x17, [x16] ; cbz x17, 1f ; br x17       // fast path once resolved
      1:  mov x9, x8 ; mov x8, #0x4153 ; svc #0        // ARMTR_NR_RESOLVE: the bridge stores the address of
          mov x8, x9 ; ldr x17, [x16] ; br x17         // the hostlibs proxy (host function) in desc.target

so args (x0-x7, v0-v7, stack) and x8 reach the hostlibs proxy untouched; the proxy returns to the caller.
Data symbols (OpenSL ES SL_IID_*) are 8-byte variables filled by an init_array constructor (ARMTR_NR_RESOLVE_DATA).
No libc dependency. Usage: gen_stubs.py OUT_DIR   (needs the AOSP clang, see build_guest.sh)
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(HERE, '..', '..', 'hostlibs', 'gen', 'hostlibs_tables.inc')
AOSP = os.environ.get('AOSP', '/mnt/storage-adata/aosp15')
CLANG = os.path.join(AOSP, 'prebuilts/clang/host/linux-x86/clang-r563880/bin/clang')
NR_RESOLVE, NR_RESOLVE_DATA = 0x4153, 0x4154
# table id -> guest sonames exporting it
LIBS = {'LOG': ['liblog.so'], 'Z': ['libz.so'], 'ANDROID': ['libandroid.so', 'libnativewindow.so'],
        'JNIGFX': ['libjnigraphics.so'], 'EGL': ['libEGL.so'], 'GLES2': ['libGLESv2.so', 'libGLESv3.so'],
        'GLES1': ['libGLESv1_CM.so'], 'SLES': ['libOpenSLES.so']}


def parse():
    tabs, cur, iids = {}, None, []
    for line in open(TABLES):
        m = re.match(r'static const Ent kTab_(\w+)\[\]', line)
        if m:
            cur = tabs.setdefault(m.group(1), [])
            continue
        if line.startswith('static const IidDef kIids'):
            cur = iids
            continue
        if line.startswith('};'):
            cur = None
            continue
        m = re.match(r'\s*\{"(\w+)"', line)
        if m and cur is not None:
            cur.append(m.group(1))
    return tabs, iids


def asm(soname, funcs, data):
    o = ['  .text']
    for i, f in enumerate(funcs):
        o += [f'  .globl {f}', f'  .type {f}, %function', '  .p2align 2', f'{f}:',
              f'  adrp x16, .Ld{i}', f'  add x16, x16, :lo12:.Ld{i}', '  ldr x17, [x16]', '  cbz x17, 1f',
              '  br x17', '1:', '  mov x9, x8', f'  mov x8, #{NR_RESOLVE}', '  svc #0', '  mov x8, x9',
              '  ldr x17, [x16]', '  br x17', f'  .size {f}, . - {f}']
    if data:
        o += ['  .p2align 2', '.Linit:']
        for i, _ in enumerate(data):
            o += [f'  adrp x16, .Lv{i}', f'  add x16, x16, :lo12:.Lv{i}', f'  mov x8, #{NR_RESOLVE_DATA}', '  svc #0']
        o += ['  ret', '  .section .init_array, "aw"', '  .p2align 3', '  .quad .Linit']
    o += ['  .data', '  .p2align 3']
    for i, f in enumerate(funcs):
        o += [f'.Ld{i}: .quad 0, .Llib, .Ln{i}']
    for i, d in enumerate(data):
        o += [f'  .globl {d}', f'  .type {d}, %object', f'  .size {d}, 8', f'{d}: .quad 0',
              f'.Lv{i}: .quad {d}, .Llib, .Lm{i}']
    o += ['  .section .rodata.str, "aMS", %progbits, 1', f'.Llib: .asciz "{soname}"']
    o += [f'.Ln{i}: .asciz "{f}"' for i, f in enumerate(funcs)]
    o += [f'.Lm{i}: .asciz "{d}"' for i, d in enumerate(data)]
    o += ['  .section .note.GNU-stack, "", %progbits']
    return '\n'.join(o) + '\n'


def main():
    out = sys.argv[1]
    tabs, iids = parse()
    with tempfile.TemporaryDirectory() as td:
        for tid, sonames in LIBS.items():
            for so in sonames:
                s = os.path.join(td, so + '.S')
                open(s, 'w').write(asm(so, tabs[tid], iids if tid == 'SLES' else []))
                subprocess.check_call([CLANG, '--target=aarch64-linux-android35', '-shared', '-nostdlib',
                                       '-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,--build-id=none',
                                       '-Wl,-soname,' + so, s, '-o', os.path.join(out, so)])
    print('stubs:', ' '.join(so for v in LIBS.values() for so in v))


if __name__ == '__main__':
    main()
