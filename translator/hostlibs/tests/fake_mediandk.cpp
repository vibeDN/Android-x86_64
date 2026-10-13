// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libmediandk + libcamera2ndk + libsync + libaaudio (one source, built under each name).
#include <stdint.h>
#include <string.h>
extern "C" {
// libmediandk
const char* AMEDIAFORMAT_KEY_MIME = "mime";
struct BufInfo { int32_t offset, size; int64_t pts; uint32_t flags; };
long AMediaCodec_dequeueOutputBuffer(void*, BufInfo* i, int64_t timeout) { i->size = 4096; i->pts = timeout * 2; return 3; }
struct ImgListener { void* ctx; void (*cb)(void*, void*); };
static ImgListener il;
int AImageReader_setImageListener(void*, ImgListener* l) { il = *l; return 0; }
void fake_media_fire_image(void* reader) { il.cb(il.ctx, reader); }
// libcamera2ndk
struct DevCbs { void* ctx; void (*disc)(void*, void*); void (*err)(void*, void*, int); void* shared; };
static DevCbs dc;
int ACameraManager_openCamera(void*, const char*, DevCbs* c, void** dev) { dc = *c; *dev = (void*)0xca3; return 0; }
void* fake_cam_shared() { return dc.shared; }
void fake_cam_fire_error(int e) { dc.err(dc.ctx, (void*)0xca3, e); }
// libsync
int sync_wait(int fd, int timeout) { return fd * 1000 + timeout; }
// libaaudio
typedef int (*DataCb)(void*, void*, void*, int32_t);
static DataCb dcb; static void* dud;
void AAudioStreamBuilder_setDataCallback(void*, DataCb cb, void* ud) { dcb = cb; dud = ud; }
int fake_aaudio_fire(void* stream, void* buf, int frames) { return dcb(stream, dud, buf, frames); }
}
