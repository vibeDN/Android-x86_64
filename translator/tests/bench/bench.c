// SPDX-License-Identifier: GPL-2.0-or-later
// armtr benchmark set (docs/translator/06-performance.md). Builds for arm64 bionic (build_bench.sh) and natively.
// usage: bench [name...]   names: sha256 lz mem malloc neon mutex atomic syscall pipe call   (default: all)
// Prints "<name> <ms> <checksum>"; the checksum must match across armtr / qemu / native.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static uint64_t rng = 88172645463325252ull;
static uint64_t xs(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

// ---- sha256 (plain C, integer heavy) ----
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
  0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
  0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
  0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
  0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
    uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e,6)^ROR(e,11)^ROR(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a,2)^ROR(a,13)^ROR(a,22)) + ((a&b)^(a&c)^(b&c));
    hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
  }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
static uint64_t b_sha256(void) {
  size_t n = 16 << 20; uint8_t* buf = malloc(n);
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 2654435761u >> 13);
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  for (size_t i = 0; i < n; i += 64) sha_block(h, buf + i);
  free(buf);
  return (uint64_t)h[0] << 32 | h[7];
}

// ---- lz: tiny LZ77 compressor with a hash table (branchy, byte loads) ----
static uint64_t b_lz(void) {
  size_t n = 8 << 20; uint8_t* in = malloc(n); uint8_t* out = malloc(n + n / 8 + 16);
  static uint32_t tab[1 << 16];
  for (size_t i = 0; i < n; i++) in[i] = (xs() % 7 == 0) ? (uint8_t)xs() : "the quick brown fox "[i % 20];
  uint64_t sum = 0;
  for (int rep = 0; rep < 3; rep++) {
    memset(tab, 0, sizeof tab);
    size_t i = 0, o = 0;
    while (i + 4 < n) {
      uint32_t v; memcpy(&v, in + i, 4);
      uint32_t hsh = (v * 2654435761u) >> 16, cand = tab[hsh]; tab[hsh] = (uint32_t)i;
      size_t len = 0;
      if (cand < i && i - cand < 65535) while (i + len < n && len < 255 && in[cand + len] == in[i + len]) len++;
      if (len >= 4) { out[o++] = 0xff; out[o++] = (uint8_t)len; out[o++] = (uint8_t)(i - cand); out[o++] = (uint8_t)((i - cand) >> 8); i += len; }
      else out[o++] = in[i++];
    }
    sum += o;
  }
  free(in); free(out);
  return sum;
}

// ---- mem: memcpy/memset of 32 MiB buffers ----
static uint64_t b_mem(void) {
  size_t n = 32 << 20; uint8_t* a = malloc(n); uint8_t* b = malloc(n);
  uint64_t s = 0;
  for (int r = 0; r < 20; r++) {  // the asm barriers keep clang from folding memcpy into memset
    memset(a, r, n); __asm__ volatile("" ::"r"(a) : "memory");
    memcpy(b, a, n); __asm__ volatile("" ::"r"(b) : "memory");
    s += b[(r * 7919) % n];
  }
  free(a); free(b);
  return s;
}

// ---- malloc churn ----
static uint64_t b_malloc(void) {
  enum { SLOTS = 4096 }; static void* p[SLOTS]; uint64_t s = 0;
  for (int i = 0; i < 2000000; i++) {
    unsigned k = xs() % SLOTS; free(p[k]);
    size_t sz = 16 + xs() % 2048; p[k] = malloc(sz); ((char*)p[k])[0] = (char)i; s += sz;
  }
  for (int k = 0; k < SLOTS; k++) free(p[k]);
  return s;
}

// ---- neon: auto-vectorized float + int8 kernels ----
static uint64_t b_neon(void) {
  enum { N = 1 << 16 };
  static float x[N], y[N]; static uint8_t u[N], v[N];
  for (int i = 0; i < N; i++) { x[i] = (float)(i & 255) * 0.5f; y[i] = 1.0f; u[i] = (uint8_t)xs(); v[i] = (uint8_t)xs(); }
  float dot = 0; uint64_t sad = 0;
  for (int r = 0; r < 1000; r++) {
    for (int i = 0; i < N; i++) y[i] = y[i] * 0.999f + x[i] * 0.001f;
    float d = 0; for (int i = 0; i < N; i++) d += x[i] * y[i];
    dot += d;
    uint32_t s = 0; for (int i = 0; i < N; i++) s += (uint32_t)abs((int)u[i] - (int)v[i]);
    sad += s; u[r % N]++;
  }
  return sad ^ (uint64_t)(dot / 1e6);
}

// ---- mutex ping-pong between 2 threads (futex contention) ----
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn; static const int ROUNDS = 50000;
static void* pong(void* a) {
  (void)a;
  for (int i = 0; i < ROUNDS; i++) {
    pthread_mutex_lock(&mu); while (turn != 1) pthread_cond_wait(&cv, &mu);
    turn = 0; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  }
  return 0;
}
static uint64_t b_mutex(void) {
  pthread_t t; pthread_create(&t, 0, pong, 0);
  for (int i = 0; i < ROUNDS; i++) {
    pthread_mutex_lock(&mu); while (turn != 0) pthread_cond_wait(&cv, &mu);
    turn = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
  }
  pthread_join(t, 0);
  return ROUNDS;
}

// ---- 4 threads: contended mutex counter + atomic adds ----
static long counter; static atomic_long acnt;
static void* contend(void* a) {
  (void)a;
  for (int i = 0; i < 200000; i++) {
    pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu);
    atomic_fetch_add(&acnt, 1);
  }
  return 0;
}
static uint64_t b_atomic(void) {
  pthread_t t[4];
  for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, contend, 0);
  for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
  return (uint64_t)counter + (uint64_t)acnt;
}

// ---- syscalls: small writes / pipe round trips ----
static uint64_t b_syscall(void) {
  int fd = open("/dev/null", O_WRONLY); char c = 'x'; uint64_t s = 0;
  for (int i = 0; i < 300000; i++) s += (uint64_t)write(fd, &c, 1);
  close(fd);
  return s;
}
static uint64_t b_pipe(void) {
  int p[2]; pipe(p); char buf[64] = {0}; uint64_t s = 0;
  for (int i = 0; i < 200000; i++) { s += (uint64_t)write(p[1], buf, 16); s += (uint64_t)read(p[0], buf, 64); }
  close(p[0]); close(p[1]);
  return s;
}

// ---- call: small non-inlined calls through function pointers (indirect branches, TB lookup) ----
typedef uint64_t (*fn_t)(uint64_t);
__attribute__((noinline)) static uint64_t f1(uint64_t x) { return x * 3 + 1; }
__attribute__((noinline)) static uint64_t f2(uint64_t x) { return x >> 1; }
__attribute__((noinline)) static uint64_t f3(uint64_t x) { return x ^ 0x5555; }
static uint64_t b_call(void) {
  volatile fn_t t[3] = {f1, f2, f3}; uint64_t x = 7;
  for (int i = 0; i < 30000000; i++) x = t[x % 3](x);
  return x;
}

struct { const char* n; uint64_t (*f)(void); } B[] = {
  {"sha256", b_sha256}, {"lz", b_lz}, {"call", b_call}, {"mem", b_mem}, {"malloc", b_malloc}, {"neon", b_neon},
  {"mutex", b_mutex}, {"atomic", b_atomic}, {"syscall", b_syscall}, {"pipe", b_pipe}};

int main(int argc, char** argv) {
  for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
    int want = argc < 2;
    for (int j = 1; j < argc; j++) want |= !strcmp(argv[j], B[i].n);
    if (!want) continue;
    rng = 88172645463325252ull;
    double t = now_ms(); uint64_t r = B[i].f(); t = now_ms() - t;
    printf("%-8s %9.1f %016llx\n", B[i].n, t, (unsigned long long)r);
    fflush(stdout);
  }
  return 0;
}
