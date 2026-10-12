// SPDX-License-Identifier: GPL-2.0-or-later
// Host-library proxies: the platform libraries real Bionic does not contain (liblog, libz, libandroid,
// libjnigraphics, libEGL, libGLESv1_CM/2/3, libOpenSLES, libnativewindow) are bound lazily to the HOST
// x86_64 Android libraries through proxy_ext.h stubs.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Matches armtr_resolver_fn (loader.h; `user` is ignored). Returns the guest address of a proxy stub (function
// symbols) or of guest-visible data (SL_IID_* variables); 0 if the symbol is not a hostlib symbol or the
// host library does not export it. lib_hint may be NULL/"" (then every table is searched, GLES2 before GLES1)
// or a DT_NEEDED name such as "libGLESv2.so".
uint64_t armtr_hostlibs_resolve(const char* lib_hint, const char* symbol, void* user);
uint64_t armtr_hostlibs_resolve2(const char* lib_hint, const char* symbol);
// Signature string of a hostlib function (proxy_ext.h syntax) or NULL if not in the tables; needs no host lib.
const char* armtr_hostlibs_sig(const char* lib_hint, const char* symbol);
// Directory tried first by dlopen (tests: fake host libs). NULL = plain library names.
void armtr_hostlibs_set_dir(const char* dir);

#ifdef __cplusplus
}
#endif
