#include "gw_capture_rules.h"
#include "gw_wire.h"
#include <limits.h>
#include <string.h>
bool gw_capture_record_valid(uint8_t type, const uint8_t *p, size_t n) {
  if (!p || n < 24 || n != 24u + gw_get_u16(p + 22))
    return false;
  if (type == 0x20) {
    if (p[8] > 3 || (p[9] & ~7u) || (p[8] & 1u && p[9] & 1u) ||
        (p[10] != 0 && p[10] != 1 && p[10] != 255) ||
        (p[19] != 0 && p[19] != 1 && p[19] != 3) || p[20] > 3 ||
        (p[21] > 15 && p[21] != 255) || (p[8] < 2 && gw_get_u16(p + 22) > 31) ||
        gw_get_u16(p + 22) > 1650)
      return false;
    if (p[10] == 255) {
      for (unsigned i = 11; i < 17; i++)
        if (p[i])
          return false;
    }
    return true; /* Malformed AD contents are intentionally retained raw. */
  }
  if (type == 0x21 || type == 0x22 || type == 0x23) {
    if (!gw_get_u32(p + 8) || (p[12] != 0 && p[12] != 1 && p[12] != 255) ||
        gw_get_u16(p + 22) > 512)
      return false;
    if (p[12] == 255) {
      for (unsigned i = 13; i < 19; i++)
        if (p[i])
          return false;
    }
    if (type == 0x21)
      return gw_get_u16(p + 19) > 0 && p[21] <= 1;
    if (type == 0x22)
      return gw_get_u16(p + 19) == 1 && p[21] == 0;
    return gw_get_u16(p + 19) == 0 && p[21] <= 2 && gw_get_u16(p + 22) == 0;
  }
  return false;
}
bool gw_capture_map_time(uint64_t capture, uint64_t nrf, uint64_t esp,
                         uint32_t error, uint64_t *mapped,
                         uint32_t *out_error) {
  uint64_t age = capture > nrf ? capture - nrf : nrf - capture;
  if (age > 60000 || error > 50)
    return false;
  if (capture >= nrf) {
    if (esp > UINT64_MAX - age)
      return false;
    *mapped = esp + age;
  } else {
    if (esp < age)
      return false;
    *mapped = esp - age;
  }
  /* Conservative 100 ppm relative drift bound plus millisecond quantization. */
  *out_error = error + 1 + (uint32_t)((age + 9999) / 10000);
  return true;
}
bool gw_capture_pairable(const gw_spool_record_t *a, const gw_spool_record_t *b,
                         uint64_t *delta) {
  if (!a || !b || a->type != 0x20 || b->type != 0x20 || a->radio == b->radio ||
      !a->mapped || !b->mapped)
    return false;
  const uint8_t *x = a->bytes, *y = b->bytes;
  if (x[10] == 255 || y[10] == 255 || x[8] != y[8] || x[10] != y[10] ||
      memcmp(x + 11, y + 11, 6) || (!(x[8] & 1u) && ((x[9] ^ y[9]) & 1u)) ||
      (x[8] >= 2 && (x[21] == 255 || y[21] == 255 || x[21] != y[21])) ||
      a->length != b->length || memcmp(x + 24, y + 24, a->length - 24))
    return false;
  uint64_t d = a->mapped_ms > b->mapped_ms ? a->mapped_ms - b->mapped_ms
                                           : b->mapped_ms - a->mapped_ms;
  if (d > 50 || (uint64_t)a->error_ms + b->error_ms > 50 - d)
    return false;
  if (delta)
    *delta = d;
  return true;
}
