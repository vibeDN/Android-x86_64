// SPDX-License-Identifier: GPL-2.0-or-later
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <string>
#include "t.h"
struct A64Stat { uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad1; int64_t size; int32_t blksize, pad2; int64_t blocks;
  int64_t at; uint64_t atn; int64_t mt; uint64_t mtn; int64_t ct; uint64_t ctn; uint32_t u4, u5; };
static const int64_t AT_FDCWD_ = -100;
static void cmp_stat(const A64Stat& a, const struct stat& h) {
  CHECK_EQ(a.dev, h.st_dev); CHECK_EQ(a.ino, h.st_ino); CHECK_EQ(a.mode, h.st_mode); CHECK_EQ(a.nlink, h.st_nlink);
  CHECK_EQ(a.uid, h.st_uid); CHECK_EQ(a.gid, h.st_gid); CHECK_EQ(a.rdev, h.st_rdev); CHECK_EQ(a.size, h.st_size);
  CHECK_EQ(a.blksize, h.st_blksize); CHECK_EQ(a.blocks, h.st_blocks); CHECK_EQ(a.at, h.st_atim.tv_sec); CHECK_EQ(a.atn, h.st_atim.tv_nsec);
  CHECK_EQ(a.mt, h.st_mtim.tv_sec); CHECK_EQ(a.mtn, h.st_mtim.tv_nsec); CHECK_EQ(a.ct, h.st_ctim.tv_sec); CHECK_EQ(a.ctn, h.st_ctim.tv_nsec);
}
int main() {
  char tmpl[] = "/tmp/armtr_sys_XXXXXX";
  std::string d = mkdtemp(tmpl);
  std::string f = d + "/file", sub = d + "/sub", ln = d + "/ln", f2 = d + "/file2";
  CHECK_EQ(sc(A64_mkdirat, AT_FDCWD_, P(sub.c_str()), 0755), 0);
  CHECK_EQ(sc(A64_mkdirat, AT_FDCWD_, P(sub.c_str()), 0755), -EEXIST);
  // openat: create + O_CLOEXEC
  int fd = (int)sc(A64_openat, AT_FDCWD_, P(f.c_str()), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  CHECK(fd >= 0);
  CHECK_EQ(sc(A64_write, fd, P("hello world"), 11), 11);
  CHECK_EQ(sc(A64_lseek, fd, 6, SEEK_SET), 6);
  char buf[32] = {};
  CHECK_EQ(sc(A64_read, fd, P(buf), 5), 5); CHECK(!strcmp(buf, "world"));
  memset(buf, 0, sizeof buf);
  CHECK_EQ(sc(A64_pread64, fd, P(buf), 5, 0), 5); CHECK(!strcmp(buf, "hello"));
  CHECK_EQ(sc(A64_pwrite64, fd, P("J"), 1, 0), 1);
  struct iovec { void* b; size_t l; } iv[2] = {{(void*)"ab", 2}, {(void*)"cd", 2}};
  CHECK_EQ(sc(A64_lseek, fd, 0, SEEK_END), 11);
  CHECK_EQ(sc(A64_writev, fd, P(iv), 2), 4);
  char b2[4], b3[11]; struct iovec rv[2] = {{b3, 11}, {b2, 4}};
  CHECK_EQ(sc(A64_preadv, fd, P(rv), 2, 0), 15); CHECK(!memcmp(b3, "Jello world", 11));
  // fstat conversion vs host
  A64Stat as; struct stat hs;
  CHECK_EQ(sc(A64_fstat, fd, P(&as)), 0); CHECK_EQ(fstat(fd, &hs), 0); cmp_stat(as, hs); CHECK_EQ(as.size, 15);
  CHECK_EQ(sc(A64_newfstatat, AT_FDCWD_, P(f.c_str()), P(&as), 0), 0); cmp_stat(as, hs);
  CHECK_EQ(sc(A64_newfstatat, AT_FDCWD_, P(d.c_str()), P(&as), 0), 0); CHECK(S_ISDIR(as.mode));
  CHECK_EQ(sc(A64_newfstatat, AT_FDCWD_, P((d + "/nope").c_str()), P(&as), 0), -ENOENT);
  CHECK_EQ(sc(A64_fstat, 9999, P(&as)), -EBADF);
  CHECK_EQ(sc(A64_fstat, fd, 8), -EFAULT);   // bad guest pointer
  CHECK_EQ(sc(A64_newfstatat, fd, P(""), P(&as), 0x1000 /*AT_EMPTY_PATH*/), 0); CHECK_EQ(as.size, 15);
  // statx passthrough
  uint8_t sx[256];
  CHECK_EQ(sc(A64_statx, AT_FDCWD_, P(f.c_str()), 0, 0x7ff, P(sx)), 0);
  // statfs
  struct statfs sf, hsf;
  CHECK_EQ(sc(A64_statfs, P(d.c_str()), P(&sf)), 0); CHECK_EQ(statfs(d.c_str(), &hsf), 0); CHECK_EQ(sf.f_type, hsf.f_type);
  // fcntl: O_* translation. aarch64 O_DIRECTORY (040000) must open a directory, and read back as the aarch64 value
  int dfd = (int)sc(A64_openat, AT_FDCWD_, P(d.c_str()), A_O_DIRECTORY | O_RDONLY, 0);
  CHECK(dfd >= 0);
  CHECK_EQ(sc(A64_openat, AT_FDCWD_, P(f.c_str()), A_O_DIRECTORY | O_RDONLY, 0), -ENOTDIR);
  int64_t fl = sc(A64_fcntl, dfd, 3);
  CHECK(fl >= 0 && (fl & A_O_DIRECTORY));   // x86 value would be 0200000 == A_O_DIRECT
  CHECK(!(fl & A_O_DIRECT));
  // O_NOFOLLOW (aarch64 0100000): symlink open fails with ELOOP
  CHECK_EQ(sc(A64_symlinkat, P(f.c_str()), AT_FDCWD_, P(ln.c_str())), 0);
  CHECK_EQ(sc(A64_openat, AT_FDCWD_, P(ln.c_str()), A_O_NOFOLLOW | O_RDONLY, 0), -ELOOP);
  CHECK(sc(A64_openat, AT_FDCWD_, P(ln.c_str()), O_RDONLY, 0) >= 0);   // closes below via fd table leak is fine
  // O_DIRECT / O_LARGEFILE accepted (tmpfs may reject O_DIRECT with EINVAL, but never ENOTDIR/ELOOP)
  int64_t r = sc(A64_openat, AT_FDCWD_, P(f.c_str()), A_O_LARGEFILE | O_RDONLY, 0); CHECK(r >= 0);
  if (r >= 0) { int64_t g = sc(A64_fcntl, r, 3); CHECK(g >= 0); sc(A64_close, r); }
  // F_SETFL with aarch64 O_APPEND|O_NONBLOCK (same bits) and F_GETFD
  CHECK_EQ(sc(A64_fcntl, fd, 4, O_APPEND | O_NONBLOCK), 0);
  CHECK(sc(A64_fcntl, fd, 3) & O_APPEND);
  CHECK(sc(A64_fcntl, fd, 1) & 1 /*FD_CLOEXEC*/);
  CHECK_EQ(sc(A64_fcntl, fd, 4, 0), 0);
  // dup / dup3
  int64_t d1 = sc(A64_dup, fd); CHECK(d1 > fd);
  CHECK_EQ(sc(A64_dup3, fd, d1, 0), d1);
  CHECK_EQ(sc(A64_dup3, fd, fd, 0), -EINVAL);
  sc(A64_close, d1);
  CHECK_EQ(sc(A64_close, d1), -EBADF);
  // pipe2 (with aarch64 O_DIRECT packet mode value) and plain
  int pfd[2];
  CHECK_EQ(sc(A64_pipe2, P(pfd), O_NONBLOCK | O_CLOEXEC), 0);
  CHECK_EQ(sc(A64_read, pfd[0], P(buf), 1), -EAGAIN);
  CHECK_EQ(sc(A64_write, pfd[1], P("x"), 1), 1);
  CHECK_EQ(sc(A64_read, pfd[0], P(buf), 1), 1);
  sc(A64_close, pfd[0]); sc(A64_close, pfd[1]);
  CHECK_EQ(sc(A64_pipe2, P(pfd), A_O_DIRECT), 0);   // packet mode
  CHECK(sc(A64_fcntl, pfd[1], 3) & A_O_DIRECT);
  sc(A64_close, pfd[0]); sc(A64_close, pfd[1]);
  // ftruncate / fallocate / fsync / flock / sendfile / copy_file_range
  CHECK_EQ(sc(A64_ftruncate, fd, 100), 0);
  CHECK_EQ(sc(A64_fsync, fd), 0); CHECK_EQ(sc(A64_fdatasync, fd), 0);
  CHECK_EQ(sc(A64_flock, fd, 2 /*LOCK_EX*/), 0); CHECK_EQ(sc(A64_flock, fd, 8), 0);
  CHECK_EQ(sc(A64_fallocate, fd, 0, 0, 200), 0);
  CHECK_EQ(fstat(fd, &hs), 0); CHECK_EQ(hs.st_size, 200);
  int fd2 = (int)sc(A64_openat, AT_FDCWD_, P(f2.c_str()), O_RDWR | O_CREAT, 0600);
  CHECK(fd2 >= 0);
  int64_t off = 0;
  CHECK_EQ(sc(A64_sendfile, fd2, fd, P(&off), 50), 50);
  CHECK_EQ(sc(A64_copy_file_range, fd, 0, fd2, 0, 10, 0), 10);
  // getdents64
  char dents[2048];
  int64_t n = sc(A64_getdents64, dfd, P(dents), sizeof dents);
  CHECK(n > 0);
  int found = 0;
  for (int64_t o = 0; o < n;) { auto* de = (struct dirent64*)(dents + o); if (!strcmp(de->d_name, "file")) found++; o += de->d_reclen; }
  CHECK_EQ(found, 1);
  // readlinkat, faccessat, chdir/getcwd, renameat2, unlinkat
  CHECK_EQ(sc(A64_readlinkat, AT_FDCWD_, P(ln.c_str()), P(buf), sizeof buf), (int64_t)f.size());
  CHECK(!memcmp(buf, f.data(), f.size()));
  CHECK_EQ(sc(A64_faccessat, AT_FDCWD_, P(f.c_str()), 4), 0);
  CHECK_EQ(sc(A64_faccessat2, AT_FDCWD_, P((d + "/nope").c_str()), 0, 0), -ENOENT);
  CHECK_EQ(sc(A64_chdir, P(d.c_str())), 0);
  char cwd[256]; CHECK(sc(A64_getcwd, P(cwd), sizeof cwd) > 0); CHECK(!strcmp(cwd, d.c_str()));
  CHECK_EQ(sc(A64_renameat2, AT_FDCWD_, P(f2.c_str()), AT_FDCWD_, P((d + "/file3").c_str()), 0), 0);
  CHECK_EQ(sc(A64_unlinkat, AT_FDCWD_, P((d + "/file3").c_str()), 0), 0);
  CHECK_EQ(sc(A64_unlinkat, AT_FDCWD_, P((d + "/file3").c_str()), 0), -ENOENT);
  CHECK_EQ(sc(A64_unlinkat, AT_FDCWD_, P(sub.c_str()), 0x200 /*AT_REMOVEDIR*/), 0);
  CHECK_EQ(sc(A64_umask, 022) >= 0, 1);
  // memfd + openat2 flags conversion
  int64_t mfd = sc(A64_memfd_create, P("t"), 0); CHECK(mfd >= 0); sc(A64_close, mfd);
  struct { uint64_t flags, mode, resolve; } how = {A_O_DIRECTORY | O_RDONLY, 0, 0};
  int64_t o2 = sc(A64_openat2, AT_FDCWD_, P(d.c_str()), P(&how), sizeof how); CHECK(o2 >= 0); sc(A64_close, o2);
  CHECK_EQ(sc(A64_openat2, AT_FDCWD_, P(d.c_str()), P(&how), 4), -EINVAL);
  // ioctl FIONREAD on a pipe
  CHECK_EQ(sc(A64_pipe2, P(pfd), 0), 0); sc(A64_write, pfd[1], P("abc"), 3);
  int nread = 0; CHECK_EQ(sc(A64_ioctl, pfd[0], 0x541B, P(&nread)), 0); CHECK_EQ(nread, 3);
  CHECK_EQ(sc(A64_ioctl, 0x7fffffff, 0x541B, P(&nread)), -EBADF);
  sc(A64_close, pfd[0]); sc(A64_close, pfd[1]);
  sc(A64_close, fd); sc(A64_close, fd2); sc(A64_close, dfd);
  std::string cmd = "rm -rf " + d; if (system(cmd.c_str())) {}
  DONE();
}
