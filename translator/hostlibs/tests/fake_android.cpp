// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libandroid.
#include <stdint.h>
extern "C" {
struct Buf { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; };
struct Rect { int32_t l, t, r, b; };
void* fake_android_env_seen; static char pix[64];
void* ANativeWindow_fromSurface(void* env, void* surf) { fake_android_env_seen = env; return (void*)((uintptr_t)env + (uintptr_t)surf); }
void* AAssetManager_fromJava(void* env, void*) { fake_android_env_seen = env; return env; }
int ANativeWindow_getWidth(void* w) { return (int)(uintptr_t)w + 1; }
int ANativeWindow_setBuffersGeometry(void*, int w, int h, int f) { return w * 100 + h * 10 + f; }
int ANativeWindow_lock(void*, Buf* b, Rect* r) {
  *b = Buf{100, 50, 128, 1, pix, {1, 2, 3, 4, 5, 6}};
  if (r) r->r = 100;
  return 0;
}
typedef int (*LooperCb)(int, int, void*);
static LooperCb lcb; static void* ldata;
int ALooper_addFd(void*, int, int, int, LooperCb cb, void* data) { lcb = cb; ldata = data; return 1; }
int fake_android_fire(int fd, int ev) { return lcb(fd, ev, ldata); }
}
