#include "gw_rules.h"
#include <ctype.h>
#include <string.h>

bool gw_rules_u64(const char *s, uint64_t *out) {
  if (!s || !*s || !out || (s[0] == '0' && s[1]))
    return false;
  uint64_t n = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
      return false;
    n = n * 10 + (unsigned)(*s - '0');
  }
  *out = n;
  return true;
}
bool gw_rules_utf8(const char *s) {
  if (!s)
    return false;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t cp;
    unsigned n;
    if (*p < 0x80) {
      if (*p < 0x20)
        return false;
      p++;
      continue;
    }
    if ((*p & 0xe0) == 0xc0) {
      cp = *p & 31;
      n = 1;
      if (cp < 2)
        return false;
    } else if ((*p & 0xf0) == 0xe0) {
      cp = *p & 15;
      n = 2;
    } else if ((*p & 0xf8) == 0xf0) {
      cp = *p & 7;
      n = 3;
    } else
      return false;
    p++;
    for (unsigned i = 0; i < n; i++) {
      if ((*p & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (*p++ & 63);
    }
    if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  return true;
}
bool gw_rules_ipv4(const char *s, uint32_t *out) {
  if (!s || !out)
    return false;
  uint32_t ip = 0;
  for (unsigned i = 0; i < 4; i++) {
    unsigned n = 0, digits = 0;
    if (s[0] == '0' && isdigit((unsigned char)s[1]))
      return false;
    while (isdigit((unsigned char)*s)) {
      n = n * 10 + (unsigned)(*s++ - '0');
      if (++digits > 3 || n > 255)
        return false;
    }
    if (!digits || (i < 3 ? *s++ != '.' : *s != 0))
      return false;
    ip = (ip << 8) | n;
  }
  if ((ip >> 24) == 0 || (ip >> 24) == 127 || (ip >> 24) >= 224)
    return false;
  *out = ip;
  return true;
}
uint32_t gw_record_crc32(const void *data, size_t size) {
  const uint8_t *p = data;
  uint32_t c = UINT32_MAX;
  while (size--) {
    c ^= *p++;
    for (unsigned b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}
void gw_record_put32(uint8_t *p, uint32_t x) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(x >> (8 * i));
}
uint32_t gw_record_get32(const uint8_t *p) {
  uint32_t x = 0;
  for (unsigned i = 0; i < 4; i++)
    x |= (uint32_t)p[i] << (8 * i);
  return x;
}
void gw_record_put64(uint8_t *p, uint64_t x) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (uint8_t)(x >> (8 * i));
}
uint64_t gw_record_get64(const uint8_t *p) {
  uint64_t x = 0;
  for (unsigned i = 0; i < 8; i++)
    x |= (uint64_t)p[i] << (8 * i);
  return x;
}
bool gw_record_valid_snapshot(const uint8_t *p, size_t n, uint64_t generation,
                              uint32_t payload_crc) {
  return p && n >= 24 && n <= 96u * 1024u + 24 && !memcmp(p, "GWSS", 4) &&
         gw_record_get32(p + 4) == n - 24 &&
         gw_record_get64(p + 8) == generation &&
         gw_record_get32(p + 20) == gw_record_crc32(p, 20) &&
         gw_record_get32(p + 16) == payload_crc &&
         payload_crc == gw_record_crc32(p + 24, n - 24);
}
bool gw_record_valid_pointer(const uint8_t *p, size_t n) {
  return p && n == 24 && !memcmp(p, "GWSA", 4) && gw_record_get32(p + 4) <= 1 &&
         gw_record_get64(p + 8) > 0 &&
         gw_record_get32(p + 20) == gw_record_crc32(p, 20);
}
