// SPDX-License-Identifier: GPL-2.0-or-later
// hostlibs unit tests: guest->host marshalling against fake host libraries, host->guest callbacks, OpenSL vtables.
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include "check.h"
#include "fake_engine.h"
#include "hostlibs.h"

using U = uint64_t;
static armtr_cpu* cpu;
static armtr_regs R;
alignas(16) static U gstack[64];
static void reset() { memset(&R, 0, sizeof R); memset(gstack, 0, sizeof gstack); R.sp = (U)gstack; }
static U call(U addr) { CHECK(addr); armtr_regs_set(cpu, &R); fake_tramp(cpu, addr); armtr_regs_get(cpu, &R); return R.x[0]; }
static void setd(int n, double d) { memset(R.v[n], 0, 16); memcpy(R.v[n], &d, 8); }
static void setf(int n, float f) { memset(R.v[n], 0, 16); memcpy(R.v[n], &f, 4); }
static U res(const char* lib, const char* s) { return armtr_hostlibs_resolve(lib, s, nullptr); }
template <class T> static T* hsym(const char* lib, const char* name) {
  void* h = dlopen((std::string(FAKEDIR) + "/" + lib).c_str(), RTLD_NOW);
  CHECK(h);
  void* s = dlsym(h, name);
  CHECK(s);
  return (T*)s;
}

struct GuestVaList { void *stack, *gr_top, *vr_top; int32_t gr_offs, vr_offs; };
struct ZS {
  const uint8_t* next_in; unsigned avail_in; unsigned long total_in; uint8_t* next_out; unsigned avail_out;
  unsigned long total_out; const char* msg; void* state; U zalloc, zfree, opaque; int data_type;
  unsigned long adler, reserved;
};
static_assert(sizeof(ZS) == 112, "z_stream layout");

static void test_tables() {
  CHECK(armtr_hostlibs_sig(nullptr, "malloc") == nullptr);
  CHECK(!strcmp(armtr_hostlibs_sig("liblog.so", "__android_log_print"), "i(ipp...)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libz.so", "deflateInit2_"), "i(piiiiipi)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libGLESv2.so", "glTexImage2D"), "v(uiiiiiuup)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libGLESv2.so", "glUniform4f"), "v(iffff)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libGLESv2.so", "glMatrixFrustumEXT"), "v(udddddd)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libGLESv2.so", "glMapBufferRange"), "p(ullu)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libGLESv1_CM.so", "glClearColorx"), "v(iiii)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libEGL.so", "eglCreateWindowSurface"), "p(pppp)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libandroid.so", "ANativeWindow_lock"), "i(ppp)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libjnigraphics.so", "AndroidBitmap_lockPixels"), "i(ppp)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libOpenSLES.so", "slCreateEngine"), "u(pupupp)"));
  CHECK(!strcmp(armtr_hostlibs_sig(nullptr, "SL_IID_ENGINE"), "data"));
  CHECK(res("libc.so", "__android_log_print") == 0);          // hint names a lib that is not ours
  CHECK(res("liblog.so", "no_such_symbol") == 0);
}

static void test_imports() {
  // every platform-lib import of the Yougram arm64 libs must be in the tables; no libc import may be claimed
  static const char* plat[] = {"ANativeWindow_fromSurface", "ANativeWindow_getFormat", "ANativeWindow_getHeight",
    "ANativeWindow_getWidth", "ANativeWindow_lock", "ANativeWindow_release", "ANativeWindow_setBuffersGeometry",
    "ANativeWindow_unlockAndPost", "AndroidBitmap_lockPixels", "AndroidBitmap_unlockPixels", "__android_log_print",
    "__android_log_vprint", "__android_log_write", "crc32", "deflate", "deflateEnd", "deflateInit2_",
    "eglGetCurrentContext", "inflate", "inflateEnd", "inflateInit2_", "slCreateEngine", "SL_IID_ANDROIDCONFIGURATION",
    "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_BUFFERQUEUE", "SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_RECORD",
    "SL_IID_VOLUME"};
  static const struct { const char* n; const char* sig; } exp[] = {
    {"ANativeWindow_fromSurface", "p(pp)"}, {"ANativeWindow_getFormat", "i(p)"}, {"ANativeWindow_getHeight", "i(p)"},
    {"ANativeWindow_getWidth", "i(p)"}, {"ANativeWindow_lock", "i(ppp)"}, {"ANativeWindow_release", "v(p)"},
    {"ANativeWindow_setBuffersGeometry", "i(piii)"}, {"ANativeWindow_unlockAndPost", "i(p)"},
    {"AndroidBitmap_lockPixels", "i(ppp)"}, {"AndroidBitmap_unlockPixels", "i(pp)"},
    {"__android_log_print", "i(ipp...)"}, {"__android_log_vprint", "i(ippp)"}, {"__android_log_write", "i(ipp)"},
    {"crc32", "l(lpu)"}, {"deflate", "i(pi)"}, {"deflateEnd", "i(p)"}, {"deflateInit2_", "i(piiiiipi)"},
    {"eglGetCurrentContext", "p()"}, {"inflate", "i(pi)"}, {"inflateEnd", "i(p)"}, {"inflateInit2_", "i(pipi)"},
    {"slCreateEngine", "u(pupupp)"}};
  std::set<std::string> want(plat, plat + sizeof plat / sizeof *plat), seen;
  std::ifstream f(IMPORTS_TXT);
  CHECK(f.good());
  std::string line; int total = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    total++;
    bool known = armtr_hostlibs_sig(nullptr, line.c_str()) != nullptr;
    if (known) seen.insert(line);
    if (known != (want.count(line) != 0)) { fprintf(stderr, "import %s: known=%d\n", line.c_str(), known); CHECK(0); }
  }
  CHECK(total >= 419 && seen == want);
  for (auto& e : exp) CHECK(!strcmp(armtr_hostlibs_sig(nullptr, e.n), e.sig));
  printf("imports: %d total, %zu platform symbols all covered\n", total, seen.size());
}

static void test_log() {
  auto* msg = hsym<char>("liblog.so", "fake_log_msg");
  auto* tag = hsym<char>("liblog.so", "fake_log_tag");
  auto* prio = hsym<int>("liblog.so", "fake_log_prio");
  U p = res("liblog.so", "__android_log_print");
  CHECK(p && p == res("liblog.so", "__android_log_print") && fake_mapped(p, 5));
  reset();
  R.x[0] = 4; R.x[1] = (U)"MyTag"; R.x[2] = (U)"n=%d s=%s d=%.2f l=%ld"; R.x[3] = 42; R.x[4] = (U)"str"; R.x[5] = 1234567890123ll;
  setd(0, 2.5);
  CHECK((int)call(p) == 7);
  CHECK(*prio == 4 && !strcmp(tag, "MyTag") && !strcmp(msg, "n=42 s=str d=2.50 l=1234567890123"));

  // va_list entry point: guest-ABI va_list {stack, gr_top, vr_top, gr_offs, vr_offs}
  uint64_t gr[6] = {99, (U)"zz", 0, 0, 0, 0};
  uint8_t vr[2][16] = {};
  double d = 0.25; memcpy(vr[0], &d, 8);
  uint64_t stk[2] = {0, 0};
  GuestVaList va = {stk, gr + 6, vr + 2, -48, -32};
  reset();
  R.x[0] = 5; R.x[1] = (U)"VT"; R.x[2] = (U)"%d/%s/%.2f"; R.x[3] = (U)&va;
  CHECK((int)call(res("liblog.so", "__android_log_vprint")) == 8);
  CHECK(*prio == 5 && !strcmp(msg, "99/zz/0.25"));
  reset(); R.x[0] = 6; R.x[1] = (U)"W"; R.x[2] = (U)"plain";
  CHECK((int)call(res("liblog.so", "__android_log_write")) == 9 && !strcmp(msg, "plain") && *prio == 6);
  reset(); R.x[0] = (U)"cond"; R.x[1] = (U)"A"; R.x[2] = (U)"assert %d"; R.x[3] = 5;   // variadic, void
  call(res("liblog.so", "__android_log_assert"));
  CHECK(!strcmp(msg, "assert 5"));
}

static unsigned long hcrc(unsigned long crc, const uint8_t* b, unsigned n) {
  unsigned long x = crc ^ 0x1234567800000000ul; while (n--) x = x * 31 + *b++; return x;
}
static U zargs_alloc_ret, zfree_ptr, zfree_opaque, zalloc_opaque; static unsigned zalloc_n, zalloc_sz;
static void test_zlib() {
  const U GA = 0x7000, GF = 0x7100;
  static char zmem[512];
  fake_guest_fn(GA, [](armtr_cpu* c) { zalloc_opaque = c->r.x[0]; zalloc_n = (unsigned)c->r.x[1]; zalloc_sz = (unsigned)c->r.x[2]; zargs_alloc_ret = (U)zmem; c->r.x[0] = (U)zmem; });
  fake_guest_fn(GF, [](armtr_cpu* c) { zfree_opaque = c->r.x[0]; zfree_ptr = c->r.x[1]; });
  ZS s; memset(&s, 0, sizeof s);
  s.zalloc = GA; s.zfree = GF; s.opaque = 0x99;
  reset();
  R.x[0] = (U)&s; R.x[1] = 6; R.x[2] = 8; R.x[3] = -15 & 0xffffffff; R.x[4] = 8; R.x[5] = 1; R.x[6] = (U)"1.3.1"; R.x[7] = 112;
  call(res("libz.so", "deflateInit2_"));
  CHECK((int)R.x[0] == 0);
  CHECK(hsym<int>("libz.so", "fake_z_args")[0] == 6 && hsym<int>("libz.so", "fake_z_args")[2] == -15 &&
        hsym<int>("libz.so", "fake_z_args")[5] == 112);                      // 8th arg + sign extension
  CHECK(!strcmp(hsym<char>("libz.so", "fake_z_version"), "1.3.1"));
  CHECK(s.zalloc != GA && s.zfree != GF);                                    // replaced by host-callable thunks
  CHECK(zalloc_opaque == 0x99 && zalloc_n == 3 && zalloc_sz == 100);         // host -> guest zalloc during init
  CHECK(*hsym<void*>("libz.so", "fake_z_alloc") == (void*)zmem);
  uint8_t in[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, out[16] = {};
  s.next_in = in; s.avail_in = 10; s.next_out = out; s.avail_out = 4;
  reset(); R.x[0] = (U)&s; R.x[1] = 0;
  call(res("libz.so", "deflate"));
  CHECK((int)R.x[0] == 0 && s.avail_in == 6 && s.total_in == 4 && s.total_out == 4 && s.next_in == in + 4 && out[3] == 4);
  s.avail_out = 12;
  reset(); R.x[0] = (U)&s; R.x[1] = 4;
  call(res("libz.so", "deflate"));
  CHECK((int)R.x[0] == 1 && s.total_out == 10 && out[9] == 10);
  reset(); R.x[0] = (U)&s; call(res("libz.so", "deflateEnd"));
  CHECK(zfree_ptr == (U)zmem && zfree_opaque == 0x99);
  // stream without allocators + inflate path + 64-bit crc argument/result
  ZS t; memset(&t, 0, sizeof t);
  reset(); R.x[0] = (U)&t; R.x[1] = 47; R.x[2] = (U)"1.3.1"; R.x[3] = 112;
  call(res("libz.so", "inflateInit2_")); CHECK((int)R.x[0] == 0 && t.zalloc == 0);
  reset(); R.x[0] = 0xdeadbeef00000001ull; R.x[1] = (U)in; R.x[2] = 10;
  CHECK(call(res("libz.so", "crc32")) == hcrc(0xdeadbeef00000001ull, in, 10));
}

static void test_android() {
  struct Env { const void* fns; void* host; } env = {nullptr, (void*)0x1000};
  reset(); R.x[0] = (U)&env; R.x[1] = 0x20;
  CHECK(call(res("libandroid.so", "ANativeWindow_fromSurface")) == 0x1020);
  CHECK(*hsym<void*>("libandroid.so", "fake_android_env_seen") == (void*)0x1000);   // guest JNIEnv -> host JNIEnv
  reset(); R.x[0] = (U)&env; R.x[1] = 0;
  CHECK(call(res("libandroid.so", "AAssetManager_fromJava")) == 0x1000);
  reset(); R.x[0] = 0x41ull | (0xffffull << 32);
  CHECK((int)call(res("libandroid.so", "ANativeWindow_getWidth")) == 0x42);
  reset(); R.x[0] = 1; R.x[1] = 3; R.x[2] = 2; R.x[3] = 0xffffffff;
  CHECK((int)call(res("libandroid.so", "ANativeWindow_setBuffersGeometry")) == 300 + 20 - 1);
  struct Buf { int32_t w, h, stride, fmt; void* bits; uint32_t res[6]; } buf = {};
  struct Rect { int32_t l, t, r, b; } rc = {};
  reset(); R.x[0] = 1; R.x[1] = (U)&buf; R.x[2] = (U)&rc;
  CHECK((int)call(res("libandroid.so", "ANativeWindow_lock")) == 0);
  CHECK(buf.w == 100 && buf.h == 50 && buf.stride == 128 && buf.bits && buf.res[5] == 6 && rc.r == 100);
  // host -> guest callback registered through ALooper_addFd
  const U CB = 0x6000; static U cb_args[3];
  fake_guest_fn(CB, [](armtr_cpu* c) { for (int i = 0; i < 3; i++) cb_args[i] = c->r.x[i]; c->r.x[0] = 77; });
  reset(); R.x[0] = 1; R.x[1] = 5; R.x[2] = -2 & 0xffffffff; R.x[3] = 1; R.x[4] = CB; R.x[5] = 0xd47a;
  CHECK((int)call(res("libandroid.so", "ALooper_addFd")) == 1);
  auto fire = hsym<int(int, int)>("libandroid.so", "fake_android_fire");
  CHECK(fire(9, 3) == 77 && cb_args[0] == 9 && cb_args[1] == 3 && cb_args[2] == 0xd47a);
  int r2 = 0;
  std::thread([&] { r2 = fire(10, 1); }).join();                      // other thread gets its own guest CPU
  CHECK(r2 == 77 && cb_args[0] == 10);
  // jnigraphics
  struct Info { uint32_t w, h, stride; int32_t fmt; uint32_t flags; } info = {};
  reset(); R.x[0] = (U)&env; R.x[1] = 300; R.x[2] = (U)&info;
  CHECK((int)call(res("libjnigraphics.so", "AndroidBitmap_getInfo")) == 0);
  CHECK(info.w == 300 && info.h == 20 && info.stride == 80 && *hsym<void*>("libjnigraphics.so", "fake_jg_env") == (void*)0x1000);
}

static void test_gl() {
  auto* gd = hsym<double>("libGLESv2.so", "fake_gl_d");
  auto* gi = hsym<long>("libGLESv2.so", "fake_gl_i");
  reset(); R.x[0] = 7; setf(0, 0.5f); setf(1, -1.25f); setf(2, 3.f); setf(3, 1e10f);
  call(res("libGLESv2.so", "glUniform4f"));
  CHECK(gi[0] == 7 && gd[0] == 0.5 && gd[1] == -1.25 && gd[2] == 3.0 && gd[3] == (double)1e10f);
  reset(); R.x[0] = 0x1701; for (int i = 0; i < 6; i++) setd(i, 0.125 * (i + 1)); setd(5, 1e300);
  call(res("libGLESv2.so", "glMatrixFrustumEXT"));
  CHECK(gi[8] == 0x1701 && gd[0] == 0.125 && gd[1] == 0.25 && gd[4] == 0.625 && gd[5] == 1e300);
  static char px[4];
  reset(); U a[] = {0xDE1, 2, 0x1908, 64, 32, 0, 0x1908, 0x1401};
  for (int i = 0; i < 8; i++) R.x[i] = a[i];
  gstack[0] = (U)px;                                                   // 9th argument on the stack
  call(res("libGLESv2.so", "glTexImage2D"));
  CHECK(gi[0] == 0xDE1 && gi[3] == 64 && gi[4] == 32 && gi[7] == 0x1401 && *hsym<const void*>("libGLESv2.so", "fake_gl_p") == px);
  static float m[16]; m[15] = 9.5f;
  reset(); R.x[0] = 3; R.x[1] = 1; R.x[2] = 0xffffff01; R.x[3] = (U)m;      // GLboolean with dirty upper bits
  call(res("libGLESv2.so", "glUniformMatrix4fv")); CHECK(gi[0] == 3 && gi[1] == 1 && gi[2] == 1 && gd[0] == 9.5);
  reset(); R.x[0] = 0x1F02;
  CHECK(!strcmp((const char*)call(res("libGLESv2.so", "glGetString")), "FakeGL 3.2"));
  long v64 = 0; reset(); R.x[0] = 1; R.x[1] = (U)&v64; call(res("libGLESv2.so", "glGetInteger64v")); CHECK(v64 == 0x123456789abcl);
  reset(); R.x[0] = 2; R.x[1] = 3; R.x[2] = 0x1406; R.x[3] = 0x100; R.x[4] = 20; R.x[5] = 16;   // GLboolean 0x100 -> false
  call(res("libGLESv2.so", "glVertexAttribPointer")); CHECK(gi[0] == 2 && gi[3] == 0 && gi[4] == 20);
  // EGL
  reset(); CHECK(call(res("libEGL.so", "eglGetCurrentContext")) == 0x1234);
  U gpa = res("libEGL.so", "eglGetProcAddress");
  reset(); R.x[0] = (U)"glMatrixFrustumEXT";
  CHECK(call(gpa) == res("libGLESv2.so", "glMatrixFrustumEXT"));
  reset(); R.x[0] = (U)"glDrawElementsInstancedEXT";
  U ext = call(gpa); CHECK(ext && ext == res("libGLESv2.so", "glDrawElementsInstancedEXT") && fake_mapped(ext, 5));
  reset(); R.x[0] = (U)"glNoSuchThing"; CHECK(call(gpa) == 0);
  // glDebugMessageCallback: GLDEBUGPROC has 7 args, called from the host
  static U da[7];
  fake_guest_fn(0x6100, [](armtr_cpu* c) { for (int i = 0; i < 7; i++) da[i] = c->r.x[i]; });
  reset(); R.x[0] = 0x6100; R.x[1] = 0x55; call(res("libGLESv2.so", "glDebugMessageCallback"));
  hsym<void(const char*)>("libGLESv2.so", "fake_gl_fire_debug")("hello");
  CHECK(da[0] == 1 && da[3] == 4 && da[4] == 5 && !strcmp((const char*)da[5], "hello") && da[6] == 0x55);
}

// ---- OpenSL ES ------------------------------------------------------------------------------------------
static U slot(U w, int i) { return ((U*)*(U*)w)[i]; }
static U sl_obj_iface(U obj, U iid_guest, U* out) { reset(); R.x[0] = obj; R.x[1] = iid_guest; R.x[2] = (U)out; return call(slot(obj, 3)) & 0xffffffff; }
static void test_sles() {
  U iid_engine = *(U*)res("libOpenSLES.so", "SL_IID_ENGINE");
  U iid_play = *(U*)res("libOpenSLES.so", "SL_IID_PLAY");
  U iid_abq = *(U*)res("libOpenSLES.so", "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
  U iid_null = *(U*)res("libOpenSLES.so", "SL_IID_NULL");
  CHECK(fake_mapped(res("libOpenSLES.so", "SL_IID_ENGINE"), 3) && fake_mapped(iid_engine, 3));
  CHECK(iid_engine != (U)*hsym<void*>("libOpenSLES.so", "SL_IID_ENGINE"));        // guest copy, not the host .rodata
  CHECK(!memcmp((void*)iid_engine, *hsym<void*>("libOpenSLES.so", "SL_IID_ENGINE"), 16));
  CHECK(res("libOpenSLES.so", "SL_IID_ENGINE") == res("libOpenSLES.so", "SL_IID_ENGINE"));

  U eng_obj = 0, ids[1] = {iid_engine}; uint32_t req[1] = {1};
  reset(); R.x[0] = (U)&eng_obj; R.x[1] = 0; R.x[2] = 0; R.x[3] = 1; R.x[4] = (U)ids; R.x[5] = (U)req;
  CHECK((uint32_t)call(res("libOpenSLES.so", "slCreateEngine")) == 0);
  CHECK(*hsym<int>("libOpenSLES.so", "fake_sl_iid_ok"));                           // IID array translated to host IIDs
  CHECK(eng_obj && eng_obj != (U)*hsym<void*>("libOpenSLES.so", "fake_sl_engine_obj") && fake_mapped(slot(eng_obj, 0), 5));
  CHECK(((U*)eng_obj)[1] == (U) * hsym<void*>("libOpenSLES.so", "fake_sl_engine_obj"));
  reset(); R.x[0] = eng_obj; R.x[1] = 1;                                            // (*obj)->Realize(obj, TRUE)
  CHECK((uint32_t)call(slot(eng_obj, 0)) == 0);
  CHECK(*hsym<void*>("libOpenSLES.so", "fake_sl_self") == *hsym<void*>("libOpenSLES.so", "fake_sl_engine_obj") &&
        *hsym<uint32_t>("libOpenSLES.so", "fake_sl_async") == 1);

  U eng = 0;
  CHECK(sl_obj_iface(eng_obj, iid_engine, &eng) == 0 && eng && eng != eng_obj);
  CHECK(((U*)eng)[1] == (U) * hsym<void*>("libOpenSLES.so", "fake_sl_engine_itf"));
  U bad = 0x1111;
  CHECK(sl_obj_iface(eng_obj, iid_null, &bad) == 12 && bad == 0);                   // interface we cannot wrap: refused

  U mix = 0;
  reset(); R.x[0] = eng; R.x[1] = (U)&mix; R.x[2] = 0; R.x[3] = 0; R.x[4] = 0;
  CHECK((uint32_t)call(slot(eng, 7)) == 0 && mix && ((U*)mix)[1] == (U) * hsym<void*>("libOpenSLES.so", "fake_sl_mix_obj"));

  struct { uint32_t type, n; } bqloc = {0x800007BD, 2};
  struct { uint32_t fmt, ch, rate, bits, cbits, mask, end; } pcm = {2, 1, 16000000, 16, 16, 4, 2};
  struct { uint32_t type; U mix; } mixloc = {4, mix};
  struct { U loc, fmt; } src = {(U)&bqloc, (U)&pcm}, snk = {(U)&mixloc, 0};
  U pids[2] = {iid_abq, iid_play}; uint32_t preq[2] = {1, 1};
  U player = 0;
  reset(); R.x[0] = eng; R.x[1] = (U)&player; R.x[2] = (U)&src; R.x[3] = (U)&snk; R.x[4] = 2; R.x[5] = (U)pids; R.x[6] = (U)preq;
  CHECK((uint32_t)call(slot(eng, 2)) == 0 && player);
  CHECK(*hsym<void*>("libOpenSLES.so", "fake_sl_snk_mix") == *hsym<void*>("libOpenSLES.so", "fake_sl_mix_obj"));  // outputMix unwrapped inside the struct
  CHECK(*hsym<int>("libOpenSLES.so", "fake_sl_iid_ok"));
  CHECK(mixloc.mix == mix);                                                         // caller's struct untouched

  U bq = 0, play = 0;
  CHECK(sl_obj_iface(player, iid_abq, &bq) == 0 && sl_obj_iface(player, iid_play, &play) == 0 && bq && play && bq != play);
  // Play::SetPlayState
  reset(); R.x[0] = play; R.x[1] = 3;
  CHECK((uint32_t)call(slot(play, 0)) == 0 && *hsym<uint32_t>("libOpenSLES.so", "fake_sl_play_state") == 3 &&
        *hsym<void*>("libOpenSLES.so", "fake_sl_self") == *hsym<void*>("libOpenSLES.so", "fake_sl_play_itf"));
  // BufferQueue::Enqueue + RegisterCallback (host -> guest, arg0 is re-wrapped)
  static char pcmbuf[320];
  reset(); R.x[0] = bq; R.x[1] = (U)pcmbuf; R.x[2] = sizeof pcmbuf;
  CHECK((uint32_t)call(slot(bq, 0)) == 0 && *hsym<const void*>("libOpenSLES.so", "fake_sl_enq_buf") == pcmbuf &&
        *hsym<uint32_t>("libOpenSLES.so", "fake_sl_enq_size") == 320);
  static U cbc[2]; const U BQCB = 0x6200;
  fake_guest_fn(BQCB, [](armtr_cpu* c) { cbc[0] = c->r.x[0]; cbc[1] = c->r.x[1]; });
  reset(); R.x[0] = bq; R.x[1] = BQCB; R.x[2] = 0xc7c;
  CHECK((uint32_t)call(slot(bq, 3)) == 0);
  hsym<void()>("libOpenSLES.so", "fake_sl_fire_bq")();
  CHECK(cbc[0] == bq && cbc[1] == 0xc7c);                                           // guest sees ITS interface pointer
  // Destroy: void return, wrapper forgotten
  reset(); R.x[0] = player; call(slot(player, 6));
  CHECK(*hsym<int>("libOpenSLES.so", "fake_sl_destroyed") == 1 &&
        *hsym<void*>("libOpenSLES.so", "fake_sl_self") == *hsym<void*>("libOpenSLES.so", "fake_sl_player_obj"));
}

// ---- NDK media / camera / sync / aaudio / vulkan ---------------------------------------------------------
static void test_ndk() {
  CHECK(!strcmp(armtr_hostlibs_sig("libmediandk.so", "AMediaCodec_dequeueOutputBuffer"), "l(ppl)"));
  CHECK(!strcmp(armtr_hostlibs_sig("libsync.so", "sync_merge"), "i(pii)"));
  CHECK(!strcmp(armtr_hostlibs_sig(nullptr, "AMEDIAFORMAT_KEY_MIME"), "data"));
  CHECK(armtr_hostlibs_sig("libcamera2ndk.so", "ACameraManager_openCamera") && armtr_hostlibs_sig("libaaudio.so", "AAudioStream_write") &&
        armtr_hostlibs_sig("libvulkan.so", "vkCmdDrawIndexed"));
  // data symbol: slot -> guest copy of the host string
  U slot = res("libmediandk.so", "AMEDIAFORMAT_KEY_MIME");
  CHECK(slot && fake_mapped(slot, 3) && !strcmp(*(const char**)slot, "mime") &&
        *(const char**)slot != *hsym<const char*>("libmediandk.so", "AMEDIAFORMAT_KEY_MIME"));
  struct { int32_t off, size; int64_t pts; uint32_t flags; } bi = {};
  reset(); R.x[0] = 1; R.x[1] = (U)&bi; R.x[2] = 5000;
  CHECK(call(res("libmediandk.so", "AMediaCodec_dequeueOutputBuffer")) == 3 && bi.size == 4096 && bi.pts == 10000);
  // AImageReader listener struct: host gets a copy with a host thunk, guest sees its context
  static U ia[2];
  fake_guest_fn(0x6300, [](armtr_cpu* c) { ia[0] = c->r.x[0]; ia[1] = c->r.x[1]; });
  U lis[2] = {0xc0ffee, 0x6300};
  reset(); R.x[0] = 0x77; R.x[1] = (U)lis;
  CHECK(call(res("libmediandk.so", "AImageReader_setImageListener")) == 0 && lis[1] == 0x6300);   // guest struct untouched
  hsym<void(void*)>("libmediandk.so", "fake_media_fire_image")((void*)0x77);
  CHECK(ia[0] == 0xc0ffee && ia[1] == 0x77);
  // camera device state callbacks (3-member struct of older apps: member 3 must reach the host as 0)
  static U ce[3];
  fake_guest_fn(0x6400, [](armtr_cpu* c) { for (int i = 0; i < 3; i++) ce[i] = c->r.x[i]; });
  U dcb[4] = {0xabc, 0x6500, 0x6400, 0xdeadbeef}; U dev = 0;
  reset(); R.x[0] = 1; R.x[1] = (U)"0"; R.x[2] = (U)dcb; R.x[3] = (U)&dev;
  CHECK(call(res("libcamera2ndk.so", "ACameraManager_openCamera")) == 0 && dev == 0xca3);
  CHECK(hsym<void*()>("libcamera2ndk.so", "fake_cam_shared")() == nullptr);
  hsym<void(int)>("libcamera2ndk.so", "fake_cam_fire_error")(4);
  CHECK(ce[0] == 0xabc && ce[1] == 0xca3 && ce[2] == 4);
  reset(); R.x[0] = 7; R.x[1] = 0xffffffff;   // timeout -1
  CHECK((int)call(res("libsync.so", "sync_wait")) == 7000 - 1);
  // AAudio data callback from a host audio thread
  static U aa[4];
  fake_guest_fn(0x6600, [](armtr_cpu* c) { for (int i = 0; i < 4; i++) aa[i] = c->r.x[i]; c->r.x[0] = 0; });
  reset(); R.x[0] = 0x55; R.x[1] = 0x6600; R.x[2] = 0x5ead;
  call(res("libaaudio.so", "AAudioStreamBuilder_setDataCallback"));
  int ar = -1;
  std::thread([&] { ar = hsym<int(void*, void*, int)>("libaaudio.so", "fake_aaudio_fire")((void*)0x99, (void*)0xb0f, 192); }).join();
  CHECK(ar == 0 && aa[0] == 0x99 && aa[1] == 0x5ead && aa[2] == 0xb0f && aa[3] == 192);

  // Vulkan: GetInstanceProcAddr returns guest-callable proxies, never host pointers
  U gipa = res("libvulkan.so", "vkGetInstanceProcAddr");
  reset(); R.x[0] = 0; R.x[1] = (U)"vkGetInstanceProcAddr"; CHECK(call(gipa) == gipa);
  reset(); R.x[0] = 0; R.x[1] = (U)"vkNoSuchFunction"; CHECK(call(gipa) == 0);
  reset(); R.x[0] = 0; R.x[1] = (U)"vkCreateInstance"; U ci_fn = call(gipa);
  CHECK(ci_fn && fake_mapped(ci_fn, 5) && ci_fn != (U)hsym<void>("libvulkan.so", "vkCreateInstance"));
  // vkCreateInstance with allocation callbacks + a debug messenger create info in pNext
  static U al[4], dm[4]; static char amem[64];
  fake_guest_fn(0x6700, [](armtr_cpu* c) { for (int i = 0; i < 4; i++) al[i] = c->r.x[i]; c->r.x[0] = (U)amem; });
  fake_guest_fn(0x6800, [](armtr_cpu* c) { for (int i = 0; i < 4; i++) dm[i] = c->r.x[i]; c->r.x[0] = 1; });
  U alloc[6] = {0xa11, 0x6700, 0, 0x6900, 0, 0};
  struct { int32_t sType; const void* next; uint32_t fl, sev, ty; U cb, ud; } dbg = {1000128004, nullptr, 0, 0x1111, 7, 0x6800, 0xd8};
  struct { int32_t sType; const void* next; } ici = {1, &dbg};
  U inst = 0;
  reset(); R.x[0] = (U)&ici; R.x[1] = (U)alloc; R.x[2] = (U)&inst;
  CHECK(call(ci_fn) == 0 && inst == 0x1757);
  CHECK(al[0] == 0xa11 && al[1] == 64 && al[2] == 16 && al[3] == 1 && *hsym<void*>("libvulkan.so", "fake_vk_allocated") == amem);
  CHECK(*hsym<int>("libvulkan.so", "fake_vk_cb_ret") == 1 && dm[0] == 0x10 && dm[1] == 1 && !strcmp((char*)dm[2], "during create") && dm[3] == 0xd8);
  CHECK(dbg.cb == 0x6800 && alloc[1] == 0x6700);                         // guest structs restored / untouched
  // extension via instance GPA + messenger callback fired later from the host
  reset(); R.x[0] = inst; R.x[1] = (U)"vkCreateDebugUtilsMessengerEXT"; U cm = call(gipa);
  CHECK(cm && fake_mapped(cm, 5));
  U msgr = 0; dbg.ud = 0xd9;
  reset(); R.x[0] = inst; R.x[1] = (U)&dbg; R.x[2] = 0; R.x[3] = (U)&msgr;
  CHECK(call(cm) == 0 && msgr == inst + 1 && dbg.cb == 0x6800);
  CHECK(hsym<uint32_t(const char*)>("libvulkan.so", "fake_vk_fire")("later") == 1 && dm[0] == 0x100 && dm[1] == 2 &&
        !strcmp((char*)dm[2], "later") && dm[3] == 0xd9);
  // device GPA: float argument through the generic proxy
  reset(); R.x[0] = 0; R.x[1] = (U)"vkGetDeviceProcAddr"; U gdpa = call(gipa);
  CHECK(gdpa && gdpa == res("libvulkan.so", "vkGetDeviceProcAddr"));
  reset(); R.x[0] = 0xde; R.x[1] = (U)"vkCmdSetLineWidth"; U lw = call(gdpa);
  CHECK(lw);
  reset(); R.x[0] = 1; setf(0, 2.5f); call(lw);
  CHECK(*hsym<void*>("libvulkan.so", "fake_vk_allocated") == (void*)10);
}

int main() {
  armtr_init();
  cpu = armtr_cpu_create(nullptr, nullptr);
  armtr_hostlibs_set_dir(FAKEDIR);
  test_tables();
  test_imports();
  test_log();
  test_zlib();
  test_android();
  test_gl();
  test_sles();
  test_ndk();
  puts("test_hostlibs OK");
  return 0;
}
