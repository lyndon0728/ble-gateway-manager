#pragma once
#include <stddef.h>
#include <stdint.h>
#define INET_ADDRSTRLEN 16
struct in_addr {
  uint32_t s_addr;
};
int inet_aton(const char *, struct in_addr *);
const char *inet_ntop(int, const void *, char *, size_t);
uint16_t htons(uint16_t);
