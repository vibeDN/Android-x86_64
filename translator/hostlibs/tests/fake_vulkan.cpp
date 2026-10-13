// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libvulkan: loader entry points, allocation callbacks and a debug-utils messenger in the pNext chain.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
extern "C" {
struct Alloc { void* ud; void* (*alloc)(void*, size_t, size_t, int); void* realloc; void (*free)(void*, void*); void* ia; void* if_; };
struct Hdr { int32_t sType; const void* pNext; };
struct DbgCI { int32_t sType; const void* pNext; uint32_t flags, sev, type; uint32_t (*cb)(int, uint32_t, const void*, void*); void* ud; };
static_assert(offsetof(DbgCI, cb) == 32, "layout");
static uint32_t (*msg_cb)(int, uint32_t, const void*, void*); static void* msg_ud;
void* fake_vk_allocated; int fake_vk_cb_ret = -1;
int vkCreateInstance(const Hdr* ci, const Alloc* a, void** out) {
  for (const Hdr* p = (const Hdr*)ci->pNext; p; p = (const Hdr*)p->pNext)
    if (p->sType == 1000128004) fake_vk_cb_ret = (int)((const DbgCI*)p)->cb(0x10, 1, "during create", ((const DbgCI*)p)->ud);
  fake_vk_allocated = a ? a->alloc(a->ud, 64, 16, 1) : nullptr;
  *out = (void*)0x1757;
  return 0;
}
static int create_messenger(void* inst, const DbgCI* ci, const Alloc*, uint64_t* out) {
  msg_cb = ci->cb; msg_ud = ci->ud; *out = (uint64_t)(uintptr_t)inst + 1; return 0;
}
static void cmd_line_width(void*, float w) { fake_vk_allocated = (void*)(uintptr_t)(w * 4); }
uint32_t fake_vk_fire(const char* m) { return msg_cb(0x100, 2, m, msg_ud); }
typedef void (*PFN)(void);
PFN vkGetInstanceProcAddr(void* inst, const char* n) {
  if (!strcmp(n, "vkCreateDebugUtilsMessengerEXT") && inst) return (PFN)create_messenger;
  if (!strcmp(n, "vkCreateInstance")) return (PFN)vkCreateInstance;
  return nullptr;
}
PFN vkGetDeviceProcAddr(void*, const char* n) { return !strcmp(n, "vkCmdSetLineWidth") ? (PFN)cmd_line_width : nullptr; }
}
