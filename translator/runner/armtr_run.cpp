// SPDX-License-Identifier: GPL-2.0-or-later
// armtr-run: run an aarch64 Linux/Bionic program (static, or dynamic with its guest linker) on x86_64
// through the armtr modules (engine + loader + syscalls + runtime). See docs/translator/04-runner.md.
//
// Usage: armtr-run [--sysroot DIR] [--host-env] [-E K=V]... [--show-cpu] prog [args...]
//   ARMTR_TRACE=1 prints every guest syscall to stderr.
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "guest_rt.h"

extern char** environ;

namespace {
constexpr size_t kMainStack = 8 << 20;
constexpr uint64_t kNeverAddr = 0x1000;   // stop address of armtr_run: guest code never gets there

[[noreturn]] void die(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void die(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "armtr-run: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
  va_end(ap);
  _exit(127);
}
}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> env = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data", "PATH=/system/bin"};
  bool show_cpu = false;
  std::string sysroot = getenv("ARMTR_SYSROOT") ? getenv("ARMTR_SYSROOT") : "";
  armtr_rt::set_trace(getenv("ARMTR_TRACE") != nullptr);
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    std::string a = argv[i];
    if (a == "--sysroot" && i + 1 < argc) sysroot = argv[++i];
    else if (a == "--host-env") { env.clear(); for (char** e = environ; *e; e++) env.push_back(*e); }
    else if (a == "-E" && i + 1 < argc) env.push_back(argv[++i]);
    else if (a == "--show-cpu") show_cpu = true;
    else if (a == "--") { i++; break; }
    else die("unknown option %s\nusage: armtr-run [--sysroot DIR] [--host-env] [-E K=V] [--show-cpu] prog [args]", a.c_str());
  }
  if (i >= argc) die("usage: armtr-run [--sysroot DIR] [--host-env] [-E K=V] [--show-cpu] prog [args]");
  armtr_rt::set_sysroot(sysroot);
  char rp[PATH_MAX];
  if (!realpath(argv[i], rp)) die("%s: %s", argv[i], strerror(errno));
  // An exe inside the sysroot is reported by its guest path (ld.config sections match on it).
  std::string self = rp;
  if (!sysroot.empty() && self.compare(0, sysroot.size() + 1, sysroot + "/") == 0) self = self.substr(sysroot.size());
  armtr_rt::set_proc_self_exe(self);
  std::vector<std::string> gargs;
  gargs.push_back(argv[i]);
  for (int k = i + 1; k < argc; k++) gargs.push_back(argv[k]);

  if (armtr_rt::init()) die("armtr_init failed");
  armtr_cpu* cpu = armtr_cpu_create_ex(armtr_syscall, nullptr, kMainStack);
  if (!cpu) die("cannot create cpu");
  armtr_rt::set_main_cpu(cpu);
  armtr_rt::set_thread_cpu(cpu);
  if (show_cpu) { uint64_t a, b; armtr_rt::probe_hwcap(cpu, &a, &b, true); }
  armtr_rt::Exec ex;
  std::string err;
  if (!armtr_rt::setup_exec(cpu, rp, rp, gargs, env, &ex, &err)) die("%s", err.c_str());
  armtr_rt::run_depth()++;
  int rc = armtr_run(cpu, kNeverAddr);
  armtr_rt::run_depth()--;
  if (rc < 0) {
    fprintf(stderr, "armtr-run: %s\n", armtr_rt::describe_fault(cpu).c_str());
    fflush(nullptr);
    _exit(139);
  }
  // main thread called plain exit(): wait for the other threads like the kernel would
  while (armtr_rt::live_threads() > 0) usleep(1000);
  fflush(nullptr);
  return armtr_rt::main_exit_code() & 0xff;
}
