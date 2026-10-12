// SPDX-License-Identifier: GPL-2.0-or-later
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "t.h"
struct AEv { uint32_t events, pad; uint64_t data; };
int main() {
  int sv[2];
  CHECK_EQ(sc(A64_socketpair, AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, P(sv)), 0);
  CHECK_EQ(sc(A64_sendto, sv[0], P("ping"), 4, 0, 0, 0), 4);
  char b[8] = {};
  CHECK_EQ(sc(A64_recvfrom, sv[1], P(b), 8, 0, 0, 0), 4); CHECK(!strcmp(b, "ping"));
  // sendmsg/recvmsg
  struct iovec { void* b; size_t l; } iv = {(void*)"msg", 3};
  struct msghdr mh = {}; mh.msg_iov = (::iovec*)&iv; mh.msg_iovlen = 1;
  CHECK_EQ(sc(A64_sendmsg, sv[0], P(&mh), 0), 3);
  char rb[8] = {}; iv = {rb, 8};
  CHECK_EQ(sc(A64_recvmsg, sv[1], P(&mh), 0), 3); CHECK(!strcmp(rb, "msg"));
  int one = 1; CHECK_EQ(sc(A64_setsockopt, sv[0], SOL_SOCKET, SO_REUSEADDR, P(&one), 4), 0);
  int v = 0; uint32_t vl = 4; CHECK_EQ(sc(A64_getsockopt, sv[0], SOL_SOCKET, SO_TYPE, P(&v), P(&vl)), 0); CHECK_EQ(v, SOCK_STREAM);
  // TCP loopback: socket/bind/listen/getsockname/connect/accept4
  int64_t ls = sc(A64_socket, AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); CHECK(ls >= 0);
  sockaddr_in sa = {}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK_EQ(sc(A64_bind, ls, P(&sa), sizeof sa), 0);
  CHECK_EQ(sc(A64_listen, ls, 4), 0);
  uint32_t sl = sizeof sa; CHECK_EQ(sc(A64_getsockname, ls, P(&sa), P(&sl)), 0); CHECK(sa.sin_port);
  int64_t cs = sc(A64_socket, AF_INET, SOCK_STREAM, 0);
  CHECK_EQ(sc(A64_connect, cs, P(&sa), sizeof sa), 0);
  int64_t as = sc(A64_accept4, ls, 0, 0, SOCK_CLOEXEC); CHECK(as >= 0);
  sockaddr_in pa; sl = sizeof pa; CHECK_EQ(sc(A64_getpeername, cs, P(&pa), P(&sl)), 0); CHECK_EQ(pa.sin_port, sa.sin_port);
  CHECK_EQ(sc(A64_shutdown, cs, 1), 0);
  CHECK_EQ(sc(A64_read, as, P(b), 8), 0);   // EOF
  sockaddr_un un = {}; un.sun_family = AF_UNIX; strcpy(un.sun_path, "/nonexistent/sock");
  CHECK_EQ(sc(A64_connect, sc(A64_socket, AF_UNIX, SOCK_STREAM, 0), P(&un), sizeof un), -ENOENT);
  CHECK_EQ(sc(A64_socket, 9999, 1, 0), -EAFNOSUPPORT);
  // epoll: aarch64 epoll_event is 16 bytes {u32, pad, u64}; x86 packed is 12
  int64_t ep = sc(A64_epoll_create1, 0x80000 /*EPOLL_CLOEXEC*/); CHECK(ep >= 0);
  AEv ev = {1 /*EPOLLIN*/, 0xdeadbeef, 0x1122334455667788ull};
  CHECK_EQ(sc(A64_epoll_ctl, ep, 1 /*ADD*/, sv[1], P(&ev)), 0);
  AEv ev2 = {1, 0, 0xCAFEull};
  int64_t efd = sc(A64_eventfd2, 0, 0); CHECK(efd >= 0);
  CHECK_EQ(sc(A64_epoll_ctl, ep, 1, efd, P(&ev2)), 0);
  CHECK_EQ(sc(A64_epoll_ctl, ep, 1, efd, P(&ev2)), -EEXIST);
  AEv out[4]; memset(out, 0xee, sizeof out);
  CHECK_EQ(sc(A64_epoll_pwait, ep, P(out), 4, 0, 0, 8), 0);   // nothing ready
  CHECK_EQ(sc(A64_sendto, sv[0], P("x"), 1, 0, 0, 0), 1);
  uint64_t one64 = 1; CHECK_EQ(sc(A64_write, efd, P(&one64), 8), 8);
  memset(out, 0xee, sizeof out);
  int64_t n = sc(A64_epoll_pwait, ep, P(out), 4, 1000, 0, 8);
  CHECK_EQ(n, 2);
  bool s1 = false, s2 = false;
  for (int i = 0; i < n; i++) {
    CHECK_EQ(out[i].events & 1, 1);
    if (out[i].data == 0x1122334455667788ull) s1 = true;
    if (out[i].data == 0xCAFEull) s2 = true;
  }
  CHECK(s1 && s2);
  CHECK(out[2].events == 0xeeeeeeee);   // no overrun past n entries
  ev.events = 4; CHECK_EQ(sc(A64_epoll_ctl, ep, 3 /*MOD*/, sv[1], P(&ev)), 0);
  CHECK_EQ(sc(A64_epoll_ctl, ep, 2 /*DEL*/, sv[1], 0), 0);
  CHECK_EQ(sc(A64_epoll_pwait, ep, P(out), 0, 0, 0, 8), -EINVAL);
  // eventfd read, timerfd
  uint64_t val = 0; CHECK_EQ(sc(A64_read, efd, P(&val), 8), 8); CHECK_EQ(val, 1);
  int64_t tfd = sc(A64_timerfd_create, CLOCK_MONOTONIC, 0); CHECK(tfd >= 0);
  struct { timespec iv, val; } its = {{0, 0}, {0, 10000000}};
  CHECK_EQ(sc(A64_timerfd_settime, tfd, 0, P(&its), 0), 0);
  CHECK_EQ(sc(A64_read, tfd, P(&val), 8), 8); CHECK_EQ(val, 1);
  // ppoll (aarch64 has only ppoll)
  struct pollfd pf = {(int)efd, POLLIN, 0};
  struct timespec to = {0, 1000000};
  CHECK_EQ(sc(A64_ppoll, P(&pf), 1, P(&to), 0, 8), 0);
  sc(A64_write, efd, P(&one64), 8);
  CHECK_EQ(sc(A64_ppoll, P(&pf), 1, P(&to), 0, 8), 1); CHECK(pf.revents & POLLIN);
  // inotify
  int64_t in = sc(A64_inotify_init1, 0x800 /*NONBLOCK*/); CHECK(in >= 0);
  CHECK(sc(A64_inotify_add_watch, in, P("/tmp"), 0x100) >= 0);
  CHECK_EQ(sc(A64_read, in, P(b), 8), -EAGAIN);
  DONE();
}
