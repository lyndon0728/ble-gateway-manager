#pragma once
#include "inet.h"
#include <stddef.h>
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define SOL_SOCKET 1
#define SO_BINDTODEVICE 1
#define SO_RCVTIMEO 2
struct ifreq {
  char ifr_name[16];
};
struct sockaddr_in {
  int sin_family;
  uint16_t sin_port;
  struct in_addr sin_addr;
};
int socket(int, int, int);
int setsockopt(int, int, int, const void *, size_t);
int bind(int, const void *, size_t);
int connect(int, const void *, size_t);
int send(int, const void *, size_t, int);
int recv(int, void *, size_t, int);
int close(int);
