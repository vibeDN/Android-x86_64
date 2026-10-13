// SPDX-License-Identifier: GPL-2.0-or-later
// armtr-run process tests (bionic arm64, built by run_tests.sh): what Go programs (syncthing, tor helpers) rely on.
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void* idle(void* p) { pause(); return p; }

int main(void) {
  // 1. main-thread stack bounds (bionic reads startstack from /proc/self/stat) contain the current sp
  pthread_attr_t at;
  void* base; size_t size;
  char here;
  pthread_getattr_np(pthread_self(), &at);
  pthread_attr_getstack(&at, &base, &size);
  printf("stack_bounds %s\n", (char*)base <= &here && &here < (char*)base + size ? "ok" : "bad");
  // 2. a fork child of a multi-threaded process ending with the plain exit syscall (Go after a failed exec)
  pthread_t t;
  pthread_create(&t, NULL, idle, NULL);
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) syscall(SYS_exit, 7);
  int st = 0;
  waitpid(pid, &st, 0);
  printf("fork_exit %s\n", WIFEXITED(st) && WEXITSTATUS(st) == 7 ? "ok" : "bad");
  fflush(stdout);
  _exit(0);
}
