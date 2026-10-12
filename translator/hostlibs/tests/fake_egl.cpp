// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
extern "C" {
static void fake_ext(void) {}
void* eglGetCurrentContext(void) { return (void*)0x1234; }
void* eglGetProcAddress(const char* n) { return !strcmp(n, "glDrawElementsInstancedEXT") ? (void*)&fake_ext : nullptr; }
}
