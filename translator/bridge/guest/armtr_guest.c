// SPDX-License-Identifier: GPL-2.0-or-later
// Guest "process" of the NativeBridge (arm64, runs under the engine). The guest linker64 loads it like any
// executable, which loads and initialises the guest libc once per app process; main() then reports to the
// bridge with a private syscall number and the bridge parks this thread (never resumed). Installed as
// /system/bin/armtr/armtr_guest. Plays the role of Berberis' guest app_process64.
#include <sys/syscall.h>
#include <unistd.h>

#define ARMTR_NR_READY 0x4152   // 'AR': not an aarch64 syscall; see translator/bridge/armtr_bridge.cpp

int main(int argc, char** argv) {
  for (;;) syscall(ARMTR_NR_READY, argc, argv);
}
