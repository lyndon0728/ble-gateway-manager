#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GW_PG_MAX 4096U
#define GW_PG_HEADER 12U
enum {
  GW_PG_MORE,
  GW_PG_COMPLETE,
  GW_PG_DUPLICATE,
  GW_PG_INVALID,
  GW_PG_EXPIRED
};
typedef struct {
  uint32_t next_sequence, sequence;
  uint16_t total, used, last_offset, last_length;
  uint8_t type, last_type;
  uint32_t last_sequence;
  uint16_t last_total;
  uint64_t deadline;
  bool active, exhausted;
  uint8_t bytes[GW_PG_MAX], last_fragment[500];
} gw_pg_rx_t;
void gw_pg_init(gw_pg_rx_t *rx);
int gw_pg_receive(gw_pg_rx_t *rx, const uint8_t *fragment, size_t length,
                  uint16_t mtu, uint64_t now_ms);
size_t gw_pg_fragment(uint8_t *out, size_t capacity, uint8_t type,
                      uint32_t sequence, const uint8_t *message, size_t total,
                      size_t offset, uint16_t mtu);
