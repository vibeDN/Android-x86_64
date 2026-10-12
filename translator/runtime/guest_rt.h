// SPDX-License-Identifier: GPL-2.0-or-later
// armtr guest runtime: what a "guest process" needs on top of engine/loader/syscalls, shared by armtr-run
// (runner/) and the NativeBridge (bridge/): execve-like image + stack setup, the syscall intercept
// (exit, set_tid_address, path mapping, /proc/self/exe), guest threads (clone -> host pthread), and the
// thread-local CPU of each host thread. See docs/translator/04-runner.md and 05-bridge.md.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "armtr/engine.h"
#include "armtr/loader.h"
#include "armtr_syscalls_ext.h"

namespace armtr_rt {

// Absolute guest paths under /system /apex /linkerconfig /vendor /product /system_ext /odm /data/local/tmp
// get this prefix (host tests: a directory laid out like the device). Empty = no mapping (on device).
void set_sysroot(const std::string& dir);
bool map_path(const char* guest, std::string* host);   // false: use the guest path unchanged
void set_trace(bool on);                                // log every guest syscall to stderr
void set_proc_self_exe(const std::string& path);        // what readlink("/proc/self/exe") returns to the guest
// Embedder hook, offered every syscall first (before the runtime's own handling). Return
// ARMTR_SYSCALL_PASS to continue. Guest calls must not be made from inside it.
void set_embedder_hook(armtr_syscall_intercept_fn fn);
// exit_group: default _exit(code). The bridge may want to log first.
int init();   // armtr_init + syscall intercept + thread hook; idempotent; 0 = ok

// The guest CPU bound to the calling host thread (guest-created threads get one automatically).
armtr_cpu* thread_cpu();
void set_thread_cpu(armtr_cpu* cpu);
// Number of armtr_run frames active on this thread (>0: a host callback is running inside guest code).
int& run_depth();
// Outermost call() frames on this thread start at this guest sp instead of the cpu's stack top (the bridge's main
// thread: libc keeps environ/auxv pointers into the top of that stack). 0 = stack top.
void set_call_stack_top(uint64_t sp);
// The next clone(CLONE_THREAD) issued on this host thread does not start a thread: the child's registers
// are stored in *out (tpidr_el0 = the new guest TLS) and the caller's own tid is returned to the guest.
void capture_next_clone(armtr_regs* out);

// Exits of a guest thread (exit syscall) stop that CPU's armtr_run; the main CPU's code is remembered.
void set_main_cpu(armtr_cpu* cpu);
int main_exit_code();
int live_threads();   // guest-created threads still running

// AT_HWCAP/AT_HWCAP2 derived from the engine's CPU model (runs mrs in the guest on `cpu`).
void probe_hwcap(armtr_cpu* cpu, uint64_t* hw1, uint64_t* hw2, bool show);

struct Exec { armtr_image* exe = nullptr; armtr_image* interp = nullptr; uint64_t entry = 0, sp = 0; };
// execve(): maps `exe_host_path` (+ its PT_INTERP, path-mapped) segments only, builds argc/argv/envp/auxv
// on the cpu's own stack and sets pc/sp. exe_guest_path goes to AT_EXECFN. false + *err on failure.
bool setup_exec(armtr_cpu* cpu, const std::string& exe_host_path, const std::string& exe_guest_path,
                const std::vector<std::string>& argv, const std::vector<std::string>& env, Exec* out,
                std::string* err);

// "guest fault: <error> (pc=.. lr=.. sp=..)  pc in <maps line>"
std::string describe_fault(armtr_cpu* cpu);

// Calls guest `entry` on `cpu` with AAPCS64 arguments: x[0..nx) integer regs, v[0..nv) FP regs (raw 64-bit
// patterns), stk[0..ns) 8-byte stack slots. Re-entrant (from a host callback running inside guest code the
// call runs below the current guest sp and all registers are restored). Returns 0 and the guest's x0 / v0
// (low 64 bits) in *x0 / *v0, or <0 on a guest fault (armtr_last_error / describe_fault).
int call(armtr_cpu* cpu, uint64_t entry, const uint64_t* x, int nx, const uint64_t* v = nullptr, int nv = 0,
         const uint64_t* stk = nullptr, int ns = 0, uint64_t* x0 = nullptr, uint64_t* v0 = nullptr);

}  // namespace armtr_rt
