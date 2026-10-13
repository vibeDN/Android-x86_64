// SPDX-License-Identifier: GPL-2.0-or-later
// armtr engine: engine.h implemented on Unicorn 2.1.4 (aarch64). See engine/README in docs/translator/.
//
// Design summary
//  * One uc_engine per armtr_cpu. The whole user address space [0x10000, 2^47-0x10000) is mapped once per
//    cpu with uc_mem_map_ptr(addr, size, ALL, host_ptr == addr): guest VA == host VA, no sync ever.
//    armtr_mem_map/protect therefore only invalidate translated code (no per-cpu remapping), and only for ranges
//    becoming executable.
//  * Performance (docs/translator/06-performance.md, Unicorn patch scripts/unicorn-armtr-perf.patch): translated
//    loads/stores access host memory directly (no softmmu TLB; ARMTR_SOFTMMU=1 restores it), TBI is done by
//    the translator (TCR TBI bits), TBs chain across pages, new code is published by IC IVAU (on_ic_ivau) or
//    by a PROT_EXEC map/protect (applied by every cpu at its next syscall).
//  * Faults: accesses outside the window (null page, kernel half) are reported by Unicorn
//    (UC_ERR_*_UNMAPPED) with exact pc/address. Accesses inside the window to pages that are unmapped
//    on the host raise a host SIGSEGV/SIGBUS inside translated code; a chained handler (active only
//    while this thread is inside uc_emu_start, not inside svc/trampoline callbacks) siglongjmps out of
//    uc_emu_start, repairs Unicorn's nesting counter and reports a guest fault (regs/pc made exact, see below).
//  * Guest signals: a fault (Unicorn-unmapped access, host SIGSEGV/SIGBUS in translated code, UDF, BRK) is
//    turned into an armtr_fault at the exact faulting instruction (cpu_restore_state from the host pc, or from
//    the helper's return address found on the abandoned host stack) and offered to the signal hook, which may
//    redirect the cpu into a guest handler; armtr_run then simply continues. armtr_kick (host signal handler)
//    sets a flag + uc_emu_stop; the hook runs after the current syscall/trampoline or when uc_emu_start returns.
//  * Trampolines: a 8-byte stub `svc #0xFFFF; ret` is written at the trampoline address; the SVC
//    interrupt hook recognises imm 0xFFFF, looks the stub up, calls the host fn and the `ret` returns to
//    x30. This stays inside one uc_emu_start (a UC_HOOK_CODE + uc_reg_write(PC) would stop emulation and
//    cost a ~4 us restart). The address must be host-writable when registered.
#include "armtr/engine.h"

#include <unicorn/unicorn.h>
extern "C" {  // uc_repair.c (Unicorn internals, pinned to 2.1.4)
int armtr_uc_nested_level(uc_engine*);
void armtr_uc_repair(uc_engine*, int nested_level);
void armtr_uc_unstop(uc_engine*);
void armtr_uc_tune(uc_engine*, int direct, void (*ic_ivau)(uc_engine*, uint64_t));
int armtr_uc_restore(uc_engine*, uintptr_t host_pc);
}

#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint64_t kWinBase = 0x10000;
constexpr uint64_t kWinEnd = (1ULL << 47) - 0x10000;
constexpr size_t kDefaultStack = 1 << 20;
constexpr size_t kPage = 4096;
constexpr uint32_t kStubSvc = 0xD4000001u | (0xFFFFu << 5);  // svc #0xffff
constexpr uint32_t kStubRet = 0xD65F03C0u;                  // ret
constexpr unsigned kExcpSwi = 2;                            // QEMU EXCP_SWI

enum { F_NONE, F_READ, F_WRITE, F_FETCH, F_OTHER };

struct Tramp { armtr_trampoline_fn fn; void* user; };
struct Range { uint64_t b, e; };

std::mutex g_mu;                      // create/destroy + invalidation log
std::shared_mutex g_tramp_mu;
std::unordered_map<uint64_t, Tramp> g_tramps;
std::vector<Range> g_log;             // invalidation log; cpu cursor = absolute index
uint64_t g_log_base = 0;              // absolute index of g_log[0]
std::atomic<uint64_t> g_log_end{0};   // g_log_base + g_log.size(), readable without g_mu
bool g_direct = true;                 // direct guest memory access (ARMTR_SOFTMMU=1: Unicorn's softmmu TLB)
constexpr size_t kLogMax = 4096;
bool g_inited = false;
struct sigaction g_old_segv, g_old_bus;
armtr_signal_fn g_sig_hook;

}  // namespace

struct armtr_cpu {
  uc_engine* uc = nullptr;
  armtr_svc_fn svc = nullptr;
  void* svc_user = nullptr;
  uint64_t stack_base = 0;   // lowest usable address (above guard page)
  size_t stack_size = 0;
  void* stack_map = nullptr;
  size_t stack_map_len = 0;
  uint64_t log_cursor = 0;
  volatile int depth = 0;
  volatile sig_atomic_t kicked = 0;   // armtr_kick: call the signal hook at the next safe point
  bool exit_req = false;     // armtr_stop: leave armtr_run
  bool failed = false;       // error pending (set by hooks)
  int fault_kind = F_NONE;
  uint64_t fault_addr = 0, fault_pc = 0;
  char err[256] = "";
};

namespace {

struct Frame {
  sigjmp_buf jb;
  armtr_cpu* cpu;
  bool active;     // true while inside Unicorn (not in a host callback)
  uint64_t addr;
  int sig, code;   // host fault
  uint64_t host_pc, host_err;
  uint64_t stk[256];  // copy of the abandoned host stack above the fault (helper return address lookup)
  Frame* prev;
};
thread_local Frame* t_top = nullptr;

struct GuestScope {  // marks host callbacks so SIGSEGV in them is not blamed on the guest
  Frame* f; bool was;
  GuestScope() : f(t_top), was(f ? f->active : false) { if (f) f->active = false; }
  ~GuestScope() { if (f) f->active = was; }
};

void chain(int sig, siginfo_t* si, void* uc, struct sigaction* old) {
  if (old->sa_flags & SA_SIGINFO) { if (old->sa_sigaction) { old->sa_sigaction(sig, si, uc); return; } }
  else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { old->sa_handler(sig); return; }
  else if (old->sa_handler == SIG_IGN) return;
  signal(sig, SIG_DFL);  // re-fault with default action
}

void on_segv(int sig, siginfo_t* si, void* uc) {
  Frame* f = t_top;
  if (f && f->active) {
    f->addr = (uint64_t)si->si_addr;
    f->sig = sig; f->code = si->si_code;
    const greg_t* g = ((ucontext_t*)uc)->uc_mcontext.gregs;
    f->host_pc = (uint64_t)g[REG_RIP]; f->host_err = (uint64_t)g[REG_ERR];
    // The stack between the fault and this Frame belongs to Unicorn (and is abandoned): keep its first words.
    uint64_t sp = (uint64_t)g[REG_RSP], end = (uint64_t)f;
    size_t n = sp < end ? (end - sp) / 8 : 0;
    if (n > 256) n = 256;
    memset(f->stk, 0, sizeof f->stk);
    memcpy(f->stk, (const void*)sp, n * 8);
    siglongjmp(f->jb, 1);
  }
  chain(sig, si, uc, sig == SIGSEGV ? &g_old_segv : &g_old_bus);
}

void set_err(armtr_cpu* c, const char* what, int kind, uint64_t addr, uint64_t pc) {
  c->failed = true;
  c->fault_kind = kind; c->fault_addr = addr; c->fault_pc = pc;
  snprintf(c->err, sizeof c->err, "%s: addr=0x%llx pc=0x%llx", what, (unsigned long long)addr,
           (unsigned long long)pc);
}

uint64_t rd(uc_engine* uc, int reg) { uint64_t v = 0; uc_reg_read(uc, reg, &v); return v; }

void sync_cache(armtr_cpu* c);

void on_intr(uc_engine* uc, uint32_t no, void* ud) {
  armtr_cpu* c = (armtr_cpu*)ud;
  uint64_t pc = rd(uc, UC_ARM64_REG_PC);
  if (no != kExcpSwi) {
    // QEMU leaves pc at the faulting insn for these. Kernel: SIGILL ILL_ILLOPC / SIGTRAP TRAP_BRKPT, si_addr=pc.
    armtr_fault ft = {0, 0, pc, 0};
    if (no == 1) ft = {4, 1, pc, 0};                                                          // EXCP_UDEF
    else if (no == 7) ft = {5, 1, pc, 0};   // EXCP_BKPT (send_user_sigtrap: no esr_context)
    else if (no == 4) ft = {7, 1, 0, 0x92000021u};          // EXCP_DATA_ABORT: alignment -> SIGBUS BUS_ADRALN
    if (ft.signo && g_sig_hook) {
      GuestScope gs;
      if (g_sig_hook(c, &ft, -1, 0)) return;
    }
    set_err(c, no == 1 ? "undefined instruction" : no == 7 ? "brk" : "unhandled exception", F_OTHER, 0, pc);
    uc_emu_stop(uc);
    return;
  }
  GuestScope gs;
  uint32_t insn = *(const uint32_t*)(pc - 4);  // identity memory; pc is already past the svc
  if (insn == kStubSvc) {
    Tramp t;
    {
      std::shared_lock<std::shared_mutex> l(g_tramp_mu);
      auto it = g_tramps.find(pc - 4);
      if (it == g_tramps.end()) { set_err(c, "unknown trampoline", F_OTHER, pc - 4, pc); uc_emu_stop(uc); return; }
      t = it->second;
    }
    t.fn(c, t.user);
    return;
  }
  static const int ids[7] = {UC_ARM64_REG_X8, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                             UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5};
  uint64_t v[7]; void* p[7];
  for (int i = 0; i < 7; i++) p[i] = &v[i];
  uc_reg_read_batch(uc, (int*)ids, p, 7);
  int64_t r = c->svc ? c->svc(c, v[0], v + 1, c->svc_user) : -38;
  uint64_t x0 = (uint64_t)r;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  sync_cache(c);   // mmap/mprotect(PROT_EXEC) by this or another thread: drop stale translations now
  if (c->kicked && g_sig_hook && !c->exit_req) {   // e.g. a host signal interrupted this syscall
    c->kicked = 0;
    g_sig_hook(c, nullptr, (int64_t)v[0], v[1]);
    if (!c->kicked) armtr_uc_unstop(uc);   // handled here: no need to leave uc_emu_start
  }
}

bool on_unmapped(uc_engine* uc, uc_mem_type t, uint64_t addr, int, int64_t, void* ud) {
  armtr_cpu* c = (armtr_cpu*)ud;
  int k = (t == UC_MEM_READ_UNMAPPED) ? F_READ : (t == UC_MEM_WRITE_UNMAPPED) ? F_WRITE : F_FETCH;
  set_err(c, k == F_READ ? "unmapped read" : k == F_WRITE ? "unmapped write" : "unmapped fetch", k, addr,
          rd(uc, UC_ARM64_REG_PC));
  // Unicorn synced pc/regs before this hook, but returning false would let the rest of the TB run on. Leave now.
  if (t_top && t_top->active && t_top->cpu == c) siglongjmp(t_top->jb, 2);
  return false;
}

// Top-byte-ignore: Linux/Android arm64 userspace ignores bits 63:56 of data addresses (bionic's scudo ORs a
// tag into every heap pointer). Unicorn's virtual TLB is an identity map without TBI, so strip the tag in a
// TLB-fill hook (called once per page miss, not per access).
bool on_tlb_fill(uc_engine*, uint64_t vaddr, uc_mem_type, uc_tlb_entry* e, void*) {
  e->paddr = (vaddr >> 55 & 1) ? vaddr : (vaddr & 0x00FFFFFFFFFFFFFFull);
  e->perms = UC_PROT_ALL;
  return true;
}

void log_range(uint64_t b, uint64_t e) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_log.size() >= kLogMax) { g_log_base += g_log.size(); g_log.clear(); }
  g_log.push_back({b, e});
  g_log_end.store(g_log_base + g_log.size());
}

// IC IVAU (direct memory mode): the guest published new code at va (one 64-byte cache line, CTR_EL0.IminLine).
// ponytail: one log entry per line; a large JIT flush overflows the log and laggard cpus do a full TB flush.
void on_ic_ivau(uc_engine* uc, uint64_t va) {
  va = (uint64_t)((int64_t)(va << 8) >> 8) & ~63ull;   // TBI
  uc_ctl_remove_cache(uc, va, va + 64);
  log_range(va, va + 64);   // other cpus: at their next syscall / run entry
}

// Apply invalidations other cpus/loader announced since this cpu last ran (cheap no-op when nothing is new).
void sync_cache(armtr_cpu* c) {
  if (c->log_cursor == g_log_end.load(std::memory_order_relaxed)) return;
  std::vector<Range> todo; bool full = false;
  {
    std::lock_guard<std::mutex> l(g_mu);
    uint64_t end = g_log_base + g_log.size();
    if (c->log_cursor == end) return;
    if (c->log_cursor < g_log_base) full = true;
    else todo.assign(g_log.begin() + (c->log_cursor - g_log_base), g_log.end());
    c->log_cursor = end;
  }
  if (full) uc_ctl_flush_tb(c->uc);
  else for (auto& r : todo) uc_ctl_remove_cache(c->uc, r.b, r.e);
}

constexpr int kXs[31] = {
#define X(n) UC_ARM64_REG_X##n
    X(0), X(1), X(2), X(3), X(4), X(5), X(6), X(7), X(8), X(9), X(10), X(11), X(12), X(13), X(14), X(15),
    X(16), X(17), X(18), X(19), X(20), X(21), X(22), X(23), X(24), X(25), X(26), X(27), X(28),
#undef X
    UC_ARM64_REG_X29, UC_ARM64_REG_X30};

}  // namespace

extern "C" {

int armtr_init(void) {
  std::lock_guard<std::mutex> l(g_mu);
  if (g_inited) return 0;
  struct sigaction sa; memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGSEGV, &sa, &g_old_segv) || sigaction(SIGBUS, &sa, &g_old_bus)) return -1;
  const char* sm = getenv("ARMTR_SOFTMMU");
  g_direct = !(sm && *sm == '1');
  g_inited = true;
  return 0;
}

armtr_cpu* armtr_cpu_create_ex(armtr_svc_fn svc, void* svc_user, size_t stack_size) {
  if (armtr_init()) return nullptr;
  if (!stack_size) stack_size = kDefaultStack;
  stack_size = (stack_size + kPage - 1) & ~(kPage - 1);
  auto* c = new armtr_cpu();
  c->svc = svc; c->svc_user = svc_user;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &c->uc) != UC_ERR_OK) { delete c; return nullptr; }
    c->log_cursor = g_log_base + g_log.size();
  }
  uc_engine* uc = c->uc;
  uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX);
  armtr_uc_tune(uc, g_direct, on_ic_ivau);
  // Top-byte-ignore in the translator (QEMU strips the tag before every data access and branch). Unicorn runs
  // the guest at EL3 (single range: TCR_EL3.TBI); TCR_EL1.TBI0/1 too in case that ever changes.
  uc_arm64_cp_reg tcr1 = {2, 0, 3, 0, 2, 3ull << 37}, tcr3 = {2, 0, 3, 6, 2, 1ull << 20};
  uc_reg_write(uc, UC_ARM64_REG_CP_REG, &tcr1);
  uc_reg_write(uc, UC_ARM64_REG_CP_REG, &tcr3);
  uc_ctl_set_tcg_buffer_size(uc, 16u << 20);
  uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);   // identity fill via on_tlb_fill (strips the top byte)
  uc_hook h;
  bool ok = uc_mem_map_ptr(uc, kWinBase, kWinEnd - kWinBase, UC_PROT_ALL, (void*)kWinBase) == UC_ERR_OK &&
            uc_hook_add(uc, &h, UC_HOOK_INTR, (void*)on_intr, c, 1, 0) == UC_ERR_OK &&
            uc_hook_add(uc, &h, UC_HOOK_TLB_FILL, (void*)on_tlb_fill, c, 1, 0) == UC_ERR_OK &&
            uc_hook_add(uc, &h, UC_HOOK_MEM_UNMAPPED, (void*)on_unmapped, c, 1, 0) == UC_ERR_OK;
  uint32_t cpacr = 3u << 20;  // FP/NEON not trapped
  ok = ok && uc_reg_write(uc, UC_ARM64_REG_CPACR_EL1, &cpacr) == UC_ERR_OK;
  c->stack_map_len = stack_size + kPage;
  c->stack_map = mmap(nullptr, c->stack_map_len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
  if (!ok || c->stack_map == MAP_FAILED) {
    if (c->stack_map == MAP_FAILED) c->stack_map = nullptr;
    armtr_cpu_destroy(c);
    return nullptr;
  }
  mprotect(c->stack_map, kPage, PROT_NONE);  // guard page at the bottom
  c->stack_base = (uint64_t)c->stack_map + kPage;
  c->stack_size = stack_size;
  uint64_t sp = c->stack_base + stack_size;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  return c;
}

armtr_cpu* armtr_cpu_create(armtr_svc_fn svc, void* svc_user) { return armtr_cpu_create_ex(svc, svc_user, 0); }

void armtr_cpu_destroy(armtr_cpu* c) {
  if (!c) return;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (c->uc) uc_close(c->uc);
  }
  if (c->stack_map) munmap(c->stack_map, c->stack_map_len);
  delete c;
}

static void inval(uint64_t addr, size_t len) { log_range(addr, addr + len); }

// The identity window already covers everything; these only drop stale translations. Only a range becoming
// executable can make a translation stale (code is never run from a non-exec mapping, and unmapped code can
// only come back through a later PROT_EXEC map/protect, which is logged then). Code rewritten in place in RWX
// memory is covered by IC IVAU (direct mode) or Unicorn's notdirty tracking (ARMTR_SOFTMMU=1).
int armtr_mem_map(uint64_t addr, size_t len, int prot) {
  if (!len || addr + len < addr) return -1;
  if (prot & 4) inval(addr, len);
  return 0;
}
int armtr_mem_unmap(uint64_t addr, size_t len) {
  if (!len || addr + len < addr) return -1;
  return 0;
}
int armtr_mem_protect(uint64_t addr, size_t len, int prot) { return armtr_mem_map(addr, len, prot); }

void armtr_regs_get(armtr_cpu* c, armtr_regs* r) {
  uc_engine* uc = c->uc;
  int ids[31 + 4 + 32]; void* p[31 + 4 + 32]; int n = 0;
  uint32_t nz = 0, fpcr = 0, fpsr = 0;
  for (int i = 0; i < 31; i++) { ids[n] = kXs[i]; p[n++] = &r->x[i]; }
  ids[n] = UC_ARM64_REG_SP; p[n++] = &r->sp;
  ids[n] = UC_ARM64_REG_PC; p[n++] = &r->pc;
  ids[n] = UC_ARM64_REG_TPIDR_EL0; p[n++] = &r->tpidr_el0;
  ids[n] = UC_ARM64_REG_NZCV; p[n++] = &nz;
  uc_reg_read_batch(uc, ids, p, n);
  n = 0;
  for (int i = 0; i < 32; i++) { ids[n] = UC_ARM64_REG_Q0 + i; p[n++] = r->v[i]; }
  ids[n] = UC_ARM64_REG_FPCR; p[n++] = &fpcr;
  ids[n] = UC_ARM64_REG_FPSR; p[n++] = &fpsr;
  uc_reg_read_batch(uc, ids, p, n);
  r->nzcv = nz; r->fpcr = fpcr; r->fpsr = fpsr;
}

void armtr_regs_set(armtr_cpu* c, const armtr_regs* r) {
  uc_engine* uc = c->uc;
  int ids[31 + 4 + 34]; void* p[31 + 4 + 34]; int n = 0;
  uint32_t nz = (uint32_t)r->nzcv, fpcr = r->fpcr, fpsr = r->fpsr;
  for (int i = 0; i < 31; i++) { ids[n] = kXs[i]; p[n++] = (void*)&r->x[i]; }
  ids[n] = UC_ARM64_REG_SP; p[n++] = (void*)&r->sp;
  ids[n] = UC_ARM64_REG_PC; p[n++] = (void*)&r->pc;
  ids[n] = UC_ARM64_REG_TPIDR_EL0; p[n++] = (void*)&r->tpidr_el0;
  ids[n] = UC_ARM64_REG_NZCV; p[n++] = &nz;
  for (int i = 0; i < 32; i++) { ids[n] = UC_ARM64_REG_Q0 + i; p[n++] = (void*)r->v[i]; }
  ids[n] = UC_ARM64_REG_FPCR; p[n++] = &fpcr;
  ids[n] = UC_ARM64_REG_FPSR; p[n++] = &fpsr;
  uc_reg_write_batch(uc, ids, p, n);
}

int armtr_trampoline_add(uint64_t addr, armtr_trampoline_fn fn, void* user) {
  if (!fn || (addr & 3) || addr < kWinBase || addr + 8 > kWinEnd) return -1;
  uint32_t* p = (uint32_t*)addr;
  p[0] = kStubSvc; p[1] = kStubRet;
  {
    std::unique_lock<std::shared_mutex> l(g_tramp_mu);
    g_tramps[addr] = {fn, user};
  }
  inval(addr, 8);
  return 0;
}

// After a host fault in translated code (Frame filled by on_segv): make regs exact and describe it like the kernel.
static armtr_fault host_fault(armtr_cpu* c, Frame& f) {
  uc_engine* uc = c->uc;
  bool exact = armtr_uc_restore(uc, f.host_pc + 2);   // the fault is in translated code itself (fast path)
  uint64_t pc = rd(uc, UC_ARM64_REG_PC);
  bool fetch = !exact && ((f.addr ^ pc) >> 12) == 0;   // translating the insn at pc touched the bad page
  if (fetch) f.addr = pc;
  // Otherwise it is in a helper called from translated code: its return address is on the host stack.
  for (int i = 0; !exact && !fetch && i < 256; i++) exact = f.stk[i] && armtr_uc_restore(uc, f.stk[i]);
  int segv_acc = f.sig == SIGSEGV && f.code == 2;   // SEGV_ACCERR
  uint64_t fsc = segv_acc ? 0x0F : 0x07;               // permission / translation fault, level 3
  uint64_t esr = fetch ? 0x82000000u | fsc : 0x92000000u | ((f.host_err & 2) ? 0x40 : 0) | fsc;
  return armtr_fault{f.sig, f.code, f.addr, esr};
}

int armtr_run(armtr_cpu* c, uint64_t stop_addr) {
  uc_engine* uc = c->uc;
  if (c->depth == 0) sync_cache(c);
  Frame f; f.cpu = c; f.active = false; f.addr = 0; f.prev = t_top;
  int saved_nested = armtr_uc_nested_level(uc);
  int rc = 0;
  c->depth++;
  c->exit_req = false;
  t_top = &f;
  for (;;) {
    c->failed = false; c->err[0] = 0; c->fault_kind = F_NONE;
    if (c->kicked && g_sig_hook) { c->kicked = 0; g_sig_hook(c, nullptr, -1, 0); }
    uint64_t pc = rd(uc, UC_ARM64_REG_PC);
    if (int jr = sigsetjmp(f.jb, 0)) {
      // Left Unicorn by siglongjmp (host fault: 1, Unicorn-unmapped access: 2): repair its bookkeeping.
      f.active = false;
      armtr_uc_repair(uc, saved_nested);
      armtr_fault ft;
      if (jr == 1) {
        ft = host_fault(c, f);
      } else {
        bool kern = c->fault_addr >> 55 & 1;   // kernel half: level 0 translation fault
        uint64_t fsc = kern ? 0x04 : 0x07;
        ft = {SIGSEGV, 1 /*SEGV_MAPERR*/, c->fault_addr,
              c->fault_kind == F_FETCH ? 0x82000000u | fsc : 0x92000000u | (c->fault_kind == F_WRITE ? 0x40 : 0) | fsc};
        uint64_t fpc = c->fault_kind == F_FETCH ? c->fault_addr : c->fault_pc;
        uc_reg_write(uc, UC_ARM64_REG_PC, &fpc);
      }
      if (g_sig_hook && g_sig_hook(c, &ft, -1, 0)) continue;
      if (jr == 1) {
        bool fetch = (ft.esr >> 26) == 0x20, wr = ft.esr & 0x40;
        int k = fetch ? F_FETCH : wr ? F_WRITE : F_READ;
        bool acc = ft.code == 2;   // SEGV_ACCERR: mapped on the host but protected
        set_err(c, fetch ? (acc ? "protected fetch" : "unmapped fetch") : wr ? (acc ? "protected write" : "unmapped write")
                         : (acc ? "protected read" : "unmapped read"), k, ft.addr, rd(uc, UC_ARM64_REG_PC));
      }
      rc = -1;
      break;
    }
    f.active = true;
    uc_err e = uc_emu_start(uc, pc, stop_addr, 0, 0);
    f.active = false;
    if (c->failed) { rc = -1; break; }
    if (e != UC_ERR_OK) {
      set_err(c, uc_strerror(e), F_OTHER, 0, rd(uc, UC_ARM64_REG_PC));
      rc = -1;
      break;
    }
    if (c->exit_req || rd(uc, UC_ARM64_REG_PC) == stop_addr) break;
    // Stopped by armtr_kick: the loop head runs the signal hook, then emulation resumes.
    // ponytail: a kick landing inside uc_emu_start's prologue (it clears stop_request) waits for the next
    // syscall/trampoline or run entry; closing that needs a Unicorn patch.
  }
  c->exit_req = false;
  t_top = f.prev;
  c->depth--;
  return rc;
}

void armtr_stop(armtr_cpu* c) { c->exit_req = true; uc_emu_stop(c->uc); }

void armtr_kick(armtr_cpu* c) {
  c->kicked = 1;
  if (c->depth > 0) uc_emu_stop(c->uc);   // plain stores (stop_request, cpu_exit): async-signal-safe
}

void armtr_set_signal_hook(armtr_signal_fn fn) { g_sig_hook = fn; }

void armtr_cpu_stack(armtr_cpu* c, uint64_t* base, size_t* size) {
  if (base) *base = c->stack_base;
  if (size) *size = c->stack_size;
}

int armtr_fault_info(armtr_cpu* c, uint64_t* addr, uint64_t* pc) {
  if (addr) *addr = c->fault_addr;
  if (pc) *pc = c->fault_pc;
  return c->fault_kind;
}

const char* armtr_last_error(armtr_cpu* c) { return c->err; }

int armtr_call(armtr_cpu* c, uint64_t entry, const uint64_t* args, int nargs, uint64_t* ret) {
  if (nargs < 0 || nargs > 8 || (nargs && !args)) return -1;
  uc_engine* uc = c->uc;
  armtr_regs saved;
  const bool nested = c->depth > 0;
  uint64_t sp;
  if (nested) {  // re-entered from a host callback: keep the outer guest state, run below its sp
    armtr_regs_get(c, &saved);
    sp = (saved.sp - 512) & ~15ULL;
  } else {
    sp = c->stack_base + c->stack_size;
  }
  uint64_t stop = c->stack_base - kPage;  // guard page address: never fetched (exit check precedes fetch)
  uint64_t xs[8] = {0};
  for (int i = 0; i < nargs; i++) xs[i] = args[i];
  int ids[11]; void* p[11];
  for (int i = 0; i < 8; i++) { ids[i] = UC_ARM64_REG_X0 + i; p[i] = &xs[i]; }
  ids[8] = UC_ARM64_REG_SP; p[8] = &sp;
  ids[9] = UC_ARM64_REG_LR; p[9] = &stop;
  ids[10] = UC_ARM64_REG_PC; p[10] = &entry;
  uc_reg_write_batch(uc, ids, p, 11);
  int rc = armtr_run(c, stop);
  if (rc == 0 && ret) *ret = rd(uc, UC_ARM64_REG_X0);
  if (nested) { armtr_regs_set(c, &saved); c->failed = false; }  // nested failure is the callee's to handle
  return rc;
}

}  // extern "C"
