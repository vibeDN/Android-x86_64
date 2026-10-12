/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Placeholder engine: always fails, so the bridge can be built/loaded (M0) without a CPU emulator. */
#include "engine.h"
int ours_engine_init(const ours_engine_hooks* h) { (void)h; return -1; }
const char* ours_engine_name(void) { return "none"; }
int ours_engine_map_identity(uint64_t a, uint64_t l, int p) { (void)a; (void)l; (void)p; return -1; }
int ours_engine_unmap(uint64_t a, uint64_t l) { (void)a; (void)l; return -1; }
int ours_engine_protect(uint64_t a, uint64_t l, int p) { (void)a; (void)l; (void)p; return -1; }
void ours_engine_flush_icache(uint64_t a, uint64_t l) { (void)a; (void)l; }
ours_engine_ctx* ours_engine_create_context(void) { return 0; }
void ours_engine_destroy_context(ours_engine_ctx* c) { (void)c; }
void ours_engine_get_regs(ours_engine_ctx* c, ours_cpu_regs* o) { (void)c; (void)o; }
void ours_engine_set_regs(ours_engine_ctx* c, const ours_cpu_regs* i) { (void)c; (void)i; }
int ours_engine_run_guest_function(ours_engine_ctx* c, uint64_t e, const uint64_t a[8], uint64_t* r) { (void)c; (void)e; (void)a; (void)r; return -1; }
int ours_engine_run_from_regs(ours_engine_ctx* c) { (void)c; return -1; }
void ours_engine_request_stop(ours_engine_ctx* c) { (void)c; }
int ours_engine_register_host_trampoline(uint64_t g, ours_trampoline_fn f, void* c, void* a) { (void)g; (void)f; (void)c; (void)a; return -1; }
