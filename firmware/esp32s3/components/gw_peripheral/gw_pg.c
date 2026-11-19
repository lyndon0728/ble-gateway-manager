#include "gw_pg.h"
#include <string.h>
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) {
  return get16(p) | (uint32_t)get16(p + 2) << 16;
}
void gw_pg_init(gw_pg_rx_t *rx) {
  memset(rx, 0, sizeof(*rx));
  rx->next_sequence = 1;
}
int gw_pg_receive(gw_pg_rx_t *r, const uint8_t *p, size_t n, uint16_t mtu,
                  uint64_t now) {
  if (r->active && now >= r->deadline) {
    r->active = false;
    return GW_PG_EXPIRED;
  }
  if (!p || n <= 12 || n > 512 || mtu < 23 || n > (size_t)mtu - 3 ||
      p[0] != 'P' || p[1] != 'G' || p[2] != 1 || p[3] > 2)
    return GW_PG_INVALID;
  uint32_t seq = get32(p + 4);
  uint16_t total = get16(p + 8), off = get16(p + 10);
  size_t part = n - 12;
  if (!seq || !total || total > GW_PG_MAX || off >= total ||
      part > (size_t)total - off)
    return GW_PG_INVALID;
  if (seq == r->last_sequence && off == r->last_offset) {
    return total == r->last_total && p[3] == r->last_type &&
                   part == r->last_length &&
                   !memcmp(p + 12, r->last_fragment, part)
               ? GW_PG_DUPLICATE
               : GW_PG_INVALID;
  }
  if (r->exhausted || seq != r->next_sequence)
    return GW_PG_INVALID;
  if (!r->active) {
    if (off)
      return GW_PG_INVALID;
    r->active = true;
    r->sequence = seq;
    r->total = total;
    r->type = p[3];
    r->used = 0;
    r->deadline = now + 5000;
  }
  if (seq != r->sequence || total != r->total || p[3] != r->type ||
      off != r->used)
    return GW_PG_INVALID;
  memcpy(r->bytes + off, p + 12, part);
  r->used += (uint16_t)part;
  r->last_sequence = seq;
  r->last_offset = off;
  r->last_length = (uint16_t)part;
  r->last_type = p[3];
  r->last_total = total;
  memcpy(r->last_fragment, p + 12, part);
  if (r->used != total)
    return GW_PG_MORE;
  r->active = false;
  if (r->next_sequence == UINT32_MAX)
    r->exhausted = true;
  else
    ++r->next_sequence;
  return GW_PG_COMPLETE;
}
size_t gw_pg_fragment(uint8_t *p, size_t cap, uint8_t type, uint32_t seq,
                      const uint8_t *data, size_t total, size_t off,
                      uint16_t mtu) {
  if (!p || !data || !seq || type > 2 || !total || total > GW_PG_MAX ||
      off >= total || mtu < 23)
    return 0;
  size_t limit = mtu - 3;
  if (limit > 512)
    limit = 512;
  if (limit > cap)
    limit = cap;
  if (limit <= 12)
    return 0;
  size_t n = total - off;
  if (n > limit - 12)
    n = limit - 12;
  p[0] = 'P';
  p[1] = 'G';
  p[2] = 1;
  p[3] = type;
  for (unsigned i = 0; i < 4; i++)
    p[4 + i] = (uint8_t)(seq >> (8 * i));
  p[8] = (uint8_t)total;
  p[9] = (uint8_t)(total >> 8);
  p[10] = (uint8_t)off;
  p[11] = (uint8_t)(off >> 8);
  memcpy(p + 12, data + off, n);
  return 12 + n;
}
