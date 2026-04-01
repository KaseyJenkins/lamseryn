#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "net_server.h"
#include "logger.h"

void net_tune_socket_buffers(int fd) {
#if !(ENABLE_SOCK_SNDBUF || ENABLE_SOCK_RCVBUF || ENABLE_TCP_NODELAY || ENABLE_TCP_NOTSENT_LOWAT)
  (void)fd;
#endif
#if ENABLE_SOCK_SNDBUF
  int snd = CONFIG_SOCK_SND_BUF;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
#endif

#if ENABLE_SOCK_RCVBUF
  int rcv = CONFIG_SOCK_RCV_BUF;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
#endif

#if ENABLE_TCP_NODELAY
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif

#if ENABLE_TCP_NOTSENT_LOWAT && defined(TCP_NOTSENT_LOWAT)
  unsigned int lowat = CONFIG_TCP_NOTSENT_LOWAT;
  setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
}

int net_set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Strict bind listener per vhost (bind + port).
static inline int is_wildcard_addr(const char *s) {
  if (!s || !*s) {
    return 1;
  }
  return (strcmp(s, "0.0.0.0") == 0) || (strcmp(s, "::") == 0) || (strcmp(s, "*") == 0);
}

int create_listening_socket_bind_port(const char *bind_addr, uint16_t port, int type, int backlog) {
  char service[6];
  snprintf(service, sizeof(service), "%u", (unsigned)port);

  struct addrinfo hints, *result = NULL, *rp;
  int sfd, optval = 1, s;

  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = type;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICSERV;

  const char *node = NULL;
  if (is_wildcard_addr(bind_addr)) {
    hints.ai_flags |= AI_PASSIVE;
    node = NULL;
  } else {
    hints.ai_flags |= AI_NUMERICHOST;
    node = bind_addr;
  }

  s = getaddrinfo(node, service, &hints, &result);
  if (s != 0) {
    return -1;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sfd == -1) {
      continue;
    }

    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
      close(sfd);
      freeaddrinfo(result);
      return -1;
    }
#ifdef SO_REUSEPORT
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
      close(sfd);
      freeaddrinfo(result);
      return -1;
    }
#endif
#ifdef TCP_DEFER_ACCEPT
    int defer_sec = CONFIG_TCP_DEFER_ACCEPT_SEC;
    const char *env_defer = getenv("TCP_DEFER_ACCEPT_SEC");
    if (env_defer && *env_defer) {
      char *endp = NULL;
      long v = strtol(env_defer, &endp, 10);
      if (endp && *endp == '\0' && v >= 0 && v <= 3600) {
        defer_sec = (int)v;
      } else {
        LOGW(LOGC_ACCEPT, "Invalid TCP_DEFER_ACCEPT_SEC=%s (keeping %d)", env_defer, defer_sec);
      }
    }
    if (defer_sec > 0) {
      if (setsockopt(sfd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_sec, sizeof(defer_sec)) == -1) {
        if (errno != ENOPROTOOPT && errno != EOPNOTSUPP) {
          LOGW(LOGC_ACCEPT, "TCP_DEFER_ACCEPT failed: %s", strerror(errno));
        }
      }
    }
#endif

    if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }

    close(sfd);
  }

  if (rp != NULL) {
    if (listen(sfd, backlog) == -1) {
      close(sfd);
      freeaddrinfo(result);
      return -1;
    }
    if (net_set_nonblock(sfd) < 0) {
      close(sfd);
      freeaddrinfo(result);
      return -1;
    }
  }

  freeaddrinfo(result);
  return (rp == NULL) ? -1 : sfd;
}
