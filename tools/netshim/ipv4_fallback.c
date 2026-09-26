// Test harness only, never shipped: an LD_PRELOAD shim that lets the engine's ENet run on a machine without IPv6.
//
// The ENet fork Oxylus uses (enet-ox, zpl-c/enet) only ever opens PF_INET6 sockets and relies on dual stack
// (IPV6_V6ONLY = 0) for IPv4 peers. A kernel with IPv6 disabled, like the container OxCity is developed in, fails
// socket(AF_INET6) with EAFNOSUPPORT, and no server or client can be created at all (see ENGINE_FEEDBACK.md).
//
// When that happens this shim hands back an AF_INET socket instead and translates at the edges: IPv4-mapped
// (::ffff:a.b.c.d), loopback (::1) and any (::) addresses become plain IPv4 on the way in, and IPv4 comes back
// IPv4-mapped on the way out, which is what ENet would have seen from a dual stack socket anyway. On a machine with
// IPv6 the first socket() call succeeds and the shim steps aside completely.
//
// Built by tools/run_net_test.sh into .sandbox/lib/, used as LD_PRELOAD=.../liboxcity_ipv4_fallback.so

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_FDS 65536
static bool shimmed[MAX_FDS];

#define REAL(name, ret, ...)                                                                                          \
  static ret (*real_##name)(__VA_ARGS__) = NULL;                                                                     \
  if (!real_##name) {                                                                                                 \
    real_##name = (ret(*)(__VA_ARGS__))dlsym(RTLD_NEXT, #name);                                                      \
  }

static bool is_shimmed(int fd) { return fd >= 0 && fd < MAX_FDS && shimmed[fd]; }

// sockaddr_in6 -> sockaddr_in, false for a real IPv6 address we can't express
static bool to_v4(const struct sockaddr* from, struct sockaddr_in* to) {
  memset(to, 0, sizeof(*to));
  to->sin_family = AF_INET;
  if (from->sa_family == AF_INET) {
    memcpy(to, from, sizeof(*to));
    return true;
  }
  if (from->sa_family != AF_INET6) {
    return false;
  }
  const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)from;
  to->sin_port = in6->sin6_port;
  if (IN6_IS_ADDR_UNSPECIFIED(&in6->sin6_addr)) {
    to->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) {
    to->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
    memcpy(&to->sin_addr.s_addr, &in6->sin6_addr.s6_addr[12], 4);
  } else {
    return false;
  }
  return true;
}

static void to_v6(const struct sockaddr_in* from, struct sockaddr_in6* to) {
  memset(to, 0, sizeof(*to));
  to->sin6_family = AF_INET6;
  to->sin6_port = from->sin_port;
  to->sin6_addr.s6_addr[10] = 0xff;
  to->sin6_addr.s6_addr[11] = 0xff;
  memcpy(&to->sin6_addr.s6_addr[12], &from->sin_addr.s_addr, 4);
}

int socket(int domain, int type, int protocol) {
  REAL(socket, int, int, int, int);
  int fd = real_socket(domain, type, protocol);
  if (fd >= 0 || domain != AF_INET6 || errno != EAFNOSUPPORT) {
    return fd;
  }
  fd = real_socket(AF_INET, type, protocol);
  if (fd >= 0 && fd < MAX_FDS) {
    shimmed[fd] = true;
  }
  return fd;
}

int close(int fd) {
  REAL(close, int, int);
  if (fd >= 0 && fd < MAX_FDS) {
    shimmed[fd] = false;
  }
  return real_close(fd);
}

int setsockopt(int fd, int level, int name, const void* value, socklen_t length) {
  REAL(setsockopt, int, int, int, int, const void*, socklen_t);
  if (is_shimmed(fd) && level == IPPROTO_IPV6) {
    return 0; // IPV6_V6ONLY and friends mean nothing on an IPv4 socket
  }
  return real_setsockopt(fd, level, name, value, length);
}

int bind(int fd, const struct sockaddr* address, socklen_t length) {
  REAL(bind, int, int, const struct sockaddr*, socklen_t);
  if (!is_shimmed(fd)) {
    return real_bind(fd, address, length);
  }
  struct sockaddr_in v4;
  if (!to_v4(address, &v4)) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return real_bind(fd, (const struct sockaddr*)&v4, sizeof(v4));
}

int getsockname(int fd, struct sockaddr* address, socklen_t* length) {
  REAL(getsockname, int, int, struct sockaddr*, socklen_t*);
  if (!is_shimmed(fd)) {
    return real_getsockname(fd, address, length);
  }
  struct sockaddr_in v4;
  socklen_t v4_length = sizeof(v4);
  const int result = real_getsockname(fd, (struct sockaddr*)&v4, &v4_length);
  if (result == 0) {
    struct sockaddr_in6 v6;
    to_v6(&v4, &v6);
    memcpy(address, &v6, *length < sizeof(v6) ? *length : sizeof(v6));
    *length = sizeof(v6);
  }
  return result;
}

ssize_t sendmsg(int fd, const struct msghdr* message, int flags) {
  REAL(sendmsg, ssize_t, int, const struct msghdr*, int);
  if (!is_shimmed(fd) || !message->msg_name) {
    return real_sendmsg(fd, message, flags);
  }
  struct sockaddr_in v4;
  if (!to_v4((const struct sockaddr*)message->msg_name, &v4)) {
    errno = ENETUNREACH;
    return -1;
  }
  struct msghdr copy = *message;
  copy.msg_name = &v4;
  copy.msg_namelen = sizeof(v4);
  return real_sendmsg(fd, &copy, flags);
}

ssize_t recvmsg(int fd, struct msghdr* message, int flags) {
  REAL(recvmsg, ssize_t, int, struct msghdr*, int);
  if (!is_shimmed(fd) || !message->msg_name) {
    return real_recvmsg(fd, message, flags);
  }
  void* caller_name = message->msg_name;
  const socklen_t caller_length = message->msg_namelen;
  struct sockaddr_in v4;
  message->msg_name = &v4;
  message->msg_namelen = sizeof(v4);
  const ssize_t result = real_recvmsg(fd, message, flags);
  message->msg_name = caller_name;
  if (result >= 0) {
    struct sockaddr_in6 v6;
    to_v6(&v4, &v6);
    memcpy(caller_name, &v6, caller_length < sizeof(v6) ? caller_length : sizeof(v6));
    message->msg_namelen = sizeof(v6);
  } else {
    message->msg_namelen = caller_length;
  }
  return result;
}
