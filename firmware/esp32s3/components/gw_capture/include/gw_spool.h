#ifndef GW_SPOOL_H
#define GW_SPOOL_H
/* Portable durable journal. All calls must be serialized by the adapter.
 * On-media integers are little endian; never persist a native C structure. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GW_SPOOL_BYTES 1048576u
#define GW_SPOOL_ROOT_BYTES 8192u
#define GW_SPOOL_ROOT_COPIES 16u
#define GW_SPOOL_SLOT_BYTES 4096u
#define GW_SPOOL_SLOTS 224u
#define GW_SPOOL_DATA_OFFSET (GW_SPOOL_ROOT_BYTES * GW_SPOOL_ROOT_COPIES)
#define GW_SPOOL_RECORD_MAX 1674u
#define GW_SPOOL_NONE 65535u
enum { GW_SP_FREE, GW_SP_PENDING, GW_SP_READY, GW_SP_QUARANTINE, GW_SP_INBOX };
enum {
  GW_SP_OK = 0,
  GW_SP_FULL = 1,
  GW_SP_FAULT = 2,
  GW_SP_REPLAY = 3,
  GW_SP_OLD = 255
};
typedef struct {
  void *ctx;
  int (*read)(void *, uint32_t, void *, size_t);
  int (*write)(void *, uint32_t, const void *, size_t);
  int (*erase)(void *, uint32_t, size_t);
  /* Inspect RAW flash: encrypted reads of erased flash are not 0xff. */
  int (*erased)(void *, uint32_t, size_t, bool *);
} gw_spool_io_t;
typedef struct {
  uint8_t boot[16], type, hash[32];
  uint32_t batch;
  uint64_t first;
  uint16_t count;
} gw_spool_watermark_t;
typedef struct {
  uint8_t type, radio, source_boot[16];
  uint16_t length;
  uint64_t arrived_ms, mapped_ms, utc_ms, order;
  uint32_t error_ms;
  bool mapped;
  uint8_t bytes[GW_SPOOL_RECORD_MAX];
} gw_spool_record_t;
typedef struct {
  uint16_t primary, peer;
} gw_spool_pair_t;
typedef struct {
  gw_spool_io_t io;
  uint8_t *root, session_boot[16];
  unsigned active;
  bool fault;
} gw_spool_t;
int gw_spool_open(gw_spool_t *, const gw_spool_io_t *,
                  const uint8_t session_boot[16]);
void gw_spool_close(gw_spool_t *);
int gw_spool_check(gw_spool_t *, const gw_spool_watermark_t *);
int gw_spool_add(gw_spool_t *, const gw_spool_record_t *, size_t,
                 const gw_spool_watermark_t *);
int gw_spool_read(gw_spool_t *, unsigned slot, gw_spool_record_t *);
int gw_spool_finish(gw_spool_t *, const gw_spool_pair_t *, size_t);
/* Caller proves RAW_PERSISTED or separate authenticated inbox custody first. */
int gw_spool_mark(gw_spool_t *, const uint16_t *slots, const uint8_t *states,
                  size_t);
unsigned gw_spool_state(const gw_spool_t *, unsigned slot);
uint64_t gw_spool_sequence(const gw_spool_t *, unsigned slot);
const uint8_t *gw_spool_event_boot(const gw_spool_t *, unsigned slot);
bool gw_spool_merged(const gw_spool_t *, unsigned slot);
size_t gw_spool_used(const gw_spool_t *);
#endif
