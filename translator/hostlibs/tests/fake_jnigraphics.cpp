// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdint.h>
extern "C" {
struct Info { uint32_t width, height, stride; int32_t format; uint32_t flags; };
void* fake_jg_env;
int AndroidBitmap_getInfo(void* env, void* bmp, Info* i) { fake_jg_env = env; *i = Info{(uint32_t)(uintptr_t)bmp, 20, 80, 1, 0}; return 0; }
}
