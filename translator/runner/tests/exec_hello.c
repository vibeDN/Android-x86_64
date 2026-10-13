// SPDX-License-Identifier: GPL-2.0-or-later
// arm64 test program for exec through binfmt_misc (device): `exec_hello` prints its argv; `exec_hello exec`
// re-executes itself (guest-side execve of an arm64 binary) with argv "child".
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
  char self[512];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  self[n > 0 ? n : 0] = 0;
  printf("hello arm64 argc=%d argv0=%s argv1=%s exe=%s\n", argc, argv[0], argc > 1 ? argv[1] : "-", self);
  fflush(stdout);
  if (argc > 1 && !strcmp(argv[1], "exec")) {
    execl(self, "exec_hello", "child", (char*)NULL);
    perror("execl");
    return 1;
  }
  return 0;
}
