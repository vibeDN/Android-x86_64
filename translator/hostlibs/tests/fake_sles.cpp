// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libOpenSLES: host objects whose interfaces are {vtable*}, as in the real library.
#include <stdint.h>
#include <string.h>
extern "C" {
struct IID { uint32_t a; uint16_t b, c, d; uint8_t n[6]; };
static const IID iid_engine = {0x8d97c260, 0xddd4, 0x11db, 0x958f, {0, 2, 0xa5, 0xd5, 0xc5, 0x1b}};
static const IID iid_play = {0xef0bd9c0, 0xddd4, 0x11db, 0xbf49, {0, 2, 0xa5, 0xd5, 0xc5, 0x1b}};
static const IID iid_abq = {0x198e4940, 0xc5d7, 0x11dd, 0xad8f, {0, 2, 0xa5, 0xd5, 0xc5, 0x1b}};
static const IID iid_unk = {1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
extern const IID* const SL_IID_ENGINE = &iid_engine;
extern const IID* const SL_IID_PLAY = &iid_play;
extern const IID* const SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &iid_abq;
extern const IID* const SL_IID_NULL = &iid_unk;      // exported but without interface wrapper support

typedef void* const* Itf;                      // like SLxxxItf: pointer to a pointer to a vtable
struct ObjVt { uint32_t (*Realize)(Itf, uint32_t); uint32_t (*Resume)(Itf, uint32_t); uint32_t (*GetState)(Itf, uint32_t*);
  uint32_t (*GetInterface)(Itf, const IID*, void*); uint32_t (*RegisterCallback)(Itf, void*, void*); void (*Abort)(Itf);
  void (*Destroy)(Itf); void* rest[3]; };
struct EngVt { void* led; void* vib; uint32_t (*CreateAudioPlayer)(Itf, Itf*, void*, void*, uint32_t, const IID* const*, const uint32_t*);
  void* rec; void* midi; void* lis; void* g3d; uint32_t (*CreateOutputMix)(Itf, Itf*, uint32_t, const IID* const*, const uint32_t*); void* rest[7]; };
struct PlayVt { uint32_t (*SetPlayState)(Itf, uint32_t); void* rest[11]; };
struct BqVt { uint32_t (*Enqueue)(Itf, const void*, uint32_t); void* Clear; void* GetState;
  uint32_t (*RegisterCallback)(Itf, void (*)(Itf, void*), void*); };

extern ObjVt obj_vt; extern EngVt eng_vt; extern PlayVt play_vt; extern BqVt bq_vt;
static const void* o_engine = &obj_vt; static const void* o_mix = &obj_vt; static const void* o_player = &obj_vt;
static const void* i_engine = &eng_vt; static const void* i_play = &play_vt; static const void* i_bq = &bq_vt;

// observations
Itf fake_sl_engine_obj = (Itf)&o_engine, fake_sl_mix_obj = (Itf)&o_mix, fake_sl_player_obj = (Itf)&o_player;
Itf fake_sl_engine_itf = (Itf)&i_engine, fake_sl_play_itf = (Itf)&i_play, fake_sl_bq_itf = (Itf)&i_bq;
Itf fake_sl_self; uint32_t fake_sl_async; int fake_sl_iid_ok; uint32_t fake_sl_play_state; int fake_sl_destroyed;
const void* fake_sl_enq_buf; uint32_t fake_sl_enq_size; Itf fake_sl_snk_mix; uint32_t fake_sl_nitf;
static void (*bqcb)(Itf, void*); static void* bqctx;

uint32_t slCreateEngine(Itf* out, uint32_t nopt, const void*, uint32_t nitf, const IID* const* ids, const uint32_t*) {
  fake_sl_nitf = nitf; fake_sl_iid_ok = nitf == 1 && ids[0] == &iid_engine && nopt == 0;
  *out = fake_sl_engine_obj; return 0;
}
static uint32_t Realize(Itf s, uint32_t a) { fake_sl_self = s; fake_sl_async = a; return 0; }
static uint32_t GetInterface(Itf s, const IID* iid, void* out) {
  fake_sl_self = s;
  if (iid == &iid_engine) { *(Itf*)out = fake_sl_engine_itf; return 0; }
  if (iid == &iid_play) { *(Itf*)out = fake_sl_play_itf; return 0; }
  if (iid == &iid_abq) { *(Itf*)out = fake_sl_bq_itf; return 0; }
  *(Itf*)out = fake_sl_bq_itf; return 0;    // unknown to the proxy: must be refused before use
}
static void Destroy(Itf s) { fake_sl_self = s; fake_sl_destroyed++; }
static uint32_t CreateOutputMix(Itf, Itf* out, uint32_t, const IID* const*, const uint32_t*) { *out = fake_sl_mix_obj; return 0; }
struct Locator { uint32_t type; Itf mix; };
struct DS { void* loc; void* fmt; };
static uint32_t CreateAudioPlayer(Itf, Itf* out, void*, void* snk, uint32_t n, const IID* const* ids, const uint32_t*) {
  fake_sl_snk_mix = ((Locator*)((DS*)snk)->loc)->mix;
  fake_sl_iid_ok = n == 2 && ids[0] == &iid_abq && ids[1] == &iid_play;
  *out = fake_sl_player_obj; return 0;
}
static uint32_t SetPlayState(Itf s, uint32_t st) { fake_sl_self = s; fake_sl_play_state = st; return 0; }
static uint32_t Enqueue(Itf s, const void* b, uint32_t n) { fake_sl_self = s; fake_sl_enq_buf = b; fake_sl_enq_size = n; return 0; }
static uint32_t BqReg(Itf, void (*cb)(Itf, void*), void* ctx) { bqcb = cb; bqctx = ctx; return 0; }
void fake_sl_fire_bq(void) { bqcb(fake_sl_bq_itf, bqctx); }
ObjVt obj_vt = {Realize, nullptr, nullptr, GetInterface, nullptr, nullptr, Destroy, {}};
EngVt eng_vt = {nullptr, nullptr, CreateAudioPlayer, nullptr, nullptr, nullptr, nullptr, CreateOutputMix, {}};
PlayVt play_vt = {SetPlayState, {}};
BqVt bq_vt = {Enqueue, nullptr, nullptr, BqReg};
}
