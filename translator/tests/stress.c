/*
 * Copyright (C) 2026 The Android-x86_64 Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/* NEON + AES/SHA/PMULL + CRC32 + float stress. Prints deterministic checksums. */
#include <arm_acle.h>
#include <arm_neon.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fnv(const void* p, size_t n) {
  const uint8_t* b = p; uint64_t h = 1469598103934665603ull;
  while (n--) { h ^= *b++; h *= 1099511628211ull; }
  return h;
}

int main(void) {
  uint8_t key[16], blk[16];
  for (int i = 0; i < 16; i++) { key[i] = i * 17; blk[i] = 255 - i; }
  /* AES */
  uint8x16_t s = vld1q_u8(blk), k = vld1q_u8(key);
  for (int r = 0; r < 10; r++) s = vaesmcq_u8(vaeseq_u8(s, k));
  vst1q_u8(blk, s);
  printf("aes    %016llx\n", (unsigned long long)fnv(blk, 16));
  /* SHA1/SHA256 */
  uint32x4_t a = vdupq_n_u32(0x67452301), b = vdupq_n_u32(0xefcdab89);
  for (int r = 0; r < 8; r++) {
    a = vsha256hq_u32(a, b, vaddq_u32(a, b));
    b = vsha256h2q_u32(b, a, vsha256su0q_u32(a, b));
    a = vsha1su1q_u32(a, b);
  }
  uint32_t o[8]; vst1q_u32(o, a); vst1q_u32(o + 4, b);
  printf("sha    %016llx\n", (unsigned long long)fnv(o, 32));
  /* PMULL */
  poly128_t p = vmull_p64(0x87654321fedcba98ull, 0x0123456789abcdefull);
  uint64_t pp[2]; memcpy(pp, &p, 16);
  printf("pmull  %016llx %016llx\n", (unsigned long long)pp[0], (unsigned long long)pp[1]);
  /* CRC32 */
  uint32_t crc = 0xffffffff;
  for (int i = 0; i < 64; i++) { crc = __crc32d(crc, 0x0123456789abcdefull * i); crc = __crc32cb(crc, i); }
  printf("crc32  %08x\n", crc);
  /* NEON int/float lanes */
  int32x4_t acc = vdupq_n_s32(0); float32x4_t f = vdupq_n_f32(1.0f);
  for (int i = 1; i < 1000; i++) {
    acc = vmlaq_s32(acc, vdupq_n_s32(i), vdupq_n_s32(i ^ 0x55));
    f = vfmaq_f32(f, vdupq_n_f32(0.001f * i), vsetq_lane_f32(i, vdupq_n_f32(0.5f), 1));
  }
  int32_t ia[4]; float fa[4];
  vst1q_s32(ia, acc); vst1q_f32(fa, f);
  printf("neon   %d %d %.6f %.6f\n", ia[0], ia[3], fa[0], fa[1]);
  /* scalar double / libm */
  double d = 0;
  for (int i = 1; i < 500; i++) d += sin(i) * cos(i / 3.0) + sqrt(i) / log(i + 1.0) + fmod(i * 1.5, 7.0);
  printf("double %.12f\n", d);
  printf("fcvt   %d %lld %u %.1f\n", (int)-3.7, (long long)1e18, (unsigned)3.9e9f, (double)(float)16777217.0);
  return 0;
}
