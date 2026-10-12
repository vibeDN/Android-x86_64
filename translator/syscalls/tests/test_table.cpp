// SPDX-License-Identifier: GPL-2.0-or-later
#include <asm/unistd.h>
#include <stdlib.h>
#include <string>
#include <set>
#include "t.h"
struct E { int nr; const char* name; int host; };
static const E kT[] = {
#include "a64_table.inc"
};
int main() {
  // hard-coded spot checks taken from include/uapi/asm-generic/unistd.h
  CHECK_EQ(A64_read, 63); CHECK_EQ(A64_write, 64); CHECK_EQ(A64_openat, 56); CHECK_EQ(A64_close, 57);
  CHECK_EQ(A64_exit, 93); CHECK_EQ(A64_exit_group, 94); CHECK_EQ(A64_clone, 220); CHECK_EQ(A64_mmap, 222);
  CHECK_EQ(A64_futex, 98); CHECK_EQ(A64_clock_gettime, 113); CHECK_EQ(A64_rt_sigaction, 134);
  CHECK_EQ(A64_socket, 198); CHECK_EQ(A64_epoll_pwait, 22); CHECK_EQ(A64_clone3, 435); CHECK_EQ(A64_getrandom, 278);
  CHECK_EQ(A64_ioctl, 29); CHECK_EQ(A64_fstat, 80); CHECK_EQ(A64_newfstatat, 79); CHECK_EQ(A64_statx, 291);
  CHECK_EQ(A64_gettid, 178); CHECK_EQ(A64_memfd_create, 279);
  // every table entry: implemented iff an x86_64 syscall of the same name exists; name round-trips
  int impl = 0;
  for (const E& e : kT) {
    const char* n = armtr_syscall_name(e.nr);
    if (e.host >= 0) { CHECK(n && !strcmp(n, e.name)); CHECK_EQ(armtr_syscall_host_nr(e.nr), e.host); impl++; }
    else CHECK(!n);
  }
  printf("%d implemented of %zu\n", impl, sizeof kT / sizeof kT[0]);
  CHECK(impl > 280);
  CHECK(!armtr_syscall_name(9999));
  // must-have list is implemented
  const char* must[] = {"openat","read","write","close","lseek","fcntl","ioctl","fstat","newfstatat","statx","statfs","getdents64",
    "readlinkat","unlinkat","mkdirat","renameat2","faccessat","faccessat2","mmap","munmap","mprotect","mremap","madvise","mlock","msync",
    "futex","clock_gettime","clock_getres","gettimeofday","nanosleep","clock_nanosleep","getpid","getppid","gettid","tgkill","kill","tkill",
    "getuid","rt_sigaction","rt_sigprocmask","sigaltstack","rt_sigreturn","prctl","set_tid_address","set_robust_list","getrandom","uname",
    "sched_yield","sched_getaffinity","sched_setaffinity","getrlimit","prlimit64","socket","connect","bind","listen","accept4","sendto",
    "recvfrom","sendmsg","recvmsg","setsockopt","getsockopt","getsockname","getpeername","shutdown","socketpair","epoll_create1","epoll_ctl",
    "epoll_pwait","eventfd2","timerfd_create","inotify_init1","ppoll","pselect6","exit","exit_group","wait4","execve","clone","clone3",
    "membarrier","memfd_create","copy_file_range","sendfile","splice","flock","fsync","fdatasync","ftruncate","fallocate","chdir","getcwd",
    "setsid","umask","capget","capset","sysinfo","times","ioprio_get","ioprio_set","pidfd_open","pidfd_send_signal","pidfd_getfd","dup","dup3","pipe2",
    "pread64","pwrite64","readv","writev"};
  for (const char* m : must) { bool f = false; for (const E& e : kT) if (!strcmp(e.name, m) && armtr_syscall_name(e.nr)) f = true; if (!f) { printf("missing %s\n", m); g_fail++; } }
  // cross-check against the kernel's own table if present
  const char* path = getenv("ARMTR_KERNEL_TBL");
  if (!path) path = "/mnt/storage-adata/kernel/common/arch/arm64/tools/syscall_64.tbl";
  if (FILE* f = fopen(path, "r")) {
    char line[512]; int rows = 0;
    while (fgets(line, sizeof line, f)) {
      int nr; char abi[32], name[64];
      if (line[0] == '#' || sscanf(line, "%d %31s %63s", &nr, abi, name) != 3) continue;
      std::string a = abi;
      if (a != "common" && a != "64" && a != "renameat" && a != "rlimit" && a != "memfd_secret") continue;
      rows++;
      const E* found = nullptr;
      for (const E& e : kT) if (e.nr == nr) found = &e;
      CHECK(found && !strcmp(found->name, name));
    }
    fclose(f);
    CHECK_EQ(rows, (int)(sizeof kT / sizeof kT[0]));
    printf("cross-checked %d rows with %s\n", rows, path);
  }
  // unknown syscall -> -ENOSYS
  CHECK_EQ(sc(9999), -ENOSYS);
  CHECK_EQ(sc(A64_lookup_dcookie), -ENOSYS);
  DONE();
}
