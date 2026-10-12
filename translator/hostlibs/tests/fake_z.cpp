// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libz: z_stream (LP64 layout) copy "compression", records init args, calls zalloc/zfree.
#include <stdint.h>
#include <string.h>
struct zs {
  const uint8_t* next_in; unsigned avail_in; unsigned long total_in; uint8_t* next_out; unsigned avail_out;
  unsigned long total_out; const char* msg; void* state;
  void* (*zalloc)(void*, unsigned, unsigned); void (*zfree)(void*, void*); void* opaque;
  int data_type; unsigned long adler, reserved;
};
extern "C" {
int fake_z_args[8]; char fake_z_version[16]; void* fake_z_alloc; int fake_z_freed;
static int init(zs* s, int a, int b, int c, int d, int e, const char* v, int sz) {
  fake_z_args[0] = a; fake_z_args[1] = b; fake_z_args[2] = c; fake_z_args[3] = d; fake_z_args[4] = e; fake_z_args[5] = sz;
  strncpy(fake_z_version, v, 15);
  if (s->zalloc) fake_z_alloc = s->zalloc(s->opaque, 3, 100);
  return 0;
}
int deflateInit2_(zs* s, int level, int method, int wbits, int mem, int strat, const char* v, int sz) { return init(s, level, method, wbits, mem, strat, v, sz); }
int inflateInit2_(zs* s, int wbits, const char* v, int sz) { return init(s, wbits, 0, 0, 0, 0, v, sz); }
static int copy(zs* s) {
  unsigned n = s->avail_in < s->avail_out ? s->avail_in : s->avail_out;
  memcpy(s->next_out, s->next_in, n);
  s->next_in += n; s->next_out += n; s->avail_in -= n; s->avail_out -= n; s->total_in += n; s->total_out += n;
  return s->avail_in ? 0 : 1;
}
int deflate(zs* s, int) { return copy(s); }
int inflate(zs* s, int) { return copy(s); }
int deflateEnd(zs* s) { if (s->zfree && fake_z_alloc) { s->zfree(s->opaque, fake_z_alloc); fake_z_freed = 1; } return 0; }
int inflateEnd(zs*) { return 0; }
unsigned long crc32(unsigned long crc, const uint8_t* b, unsigned n) { unsigned long x = crc ^ 0x1234567800000000ul; while (n--) x = x * 31 + *b++; return x; }
}
