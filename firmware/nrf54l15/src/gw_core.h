#ifndef GW_CORE_H
#define GW_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PAYLOAD_MAX 3072U
#define GW_RAW_MAX 3094U
#define GW_ENCODED_MAX 3107U
#define GW_WIRE_MAX 3109U
#define GW_BATCH_MAX 2048U
#define GW_QUEUE_RECORDS 128U
#define GW_QUEUE_BYTES 32768U
#define GW_UUIDS_MAX 128U
#define GW_RULES_MAX 8U
#define GW_PROFILE_MAX 2755U

enum gw_status {
    GW_OK = 0, GW_BAD_PARAM = 1, GW_UNSUPPORTED = 2, GW_BUSY = 3,
    GW_CONFLICT = 4, GW_APPLY_FAILED = 5, GW_STALE = 6, GW_NOT_FOUND = 7
};

struct gw_frame {
    uint8_t type;
    uint64_t link_id;
    uint32_t msg_id;
    uint16_t payload_len;
    uint8_t payload[GW_PAYLOAD_MAX];
};

struct gw_decoder {
    uint8_t encoded[GW_ENCODED_MAX];
    size_t length;
    bool discard;
};

uint16_t gw_get_u16(const uint8_t *p);
uint32_t gw_get_u32(const uint8_t *p);
uint64_t gw_get_u64(const uint8_t *p);
void gw_put_u16(uint8_t *p, uint16_t value);
void gw_put_u32(uint8_t *p, uint32_t value);
void gw_put_u64(uint8_t *p, uint64_t value);
uint32_t gw_crc32c(const uint8_t *data, size_t length);
/* Returns encoded length including both delimiters, or zero on invalid input. */
size_t gw_encode_frame(const struct gw_frame *frame, uint8_t *wire, size_t capacity);
/* Decode one COBS segment, without the zero delimiters. */
bool gw_decode_frame(const uint8_t *encoded, size_t length, struct gw_frame *frame);
void gw_decoder_init(struct gw_decoder *decoder);
/* 1 = complete frame, 0 = incomplete/empty, -1 = one invalid segment. */
int gw_decoder_feed(struct gw_decoder *decoder, uint8_t byte, struct gw_frame *frame);

struct gw_beacon_rule {
    uint8_t action;
    uint8_t kind_mask;
    uint8_t ad_type;
    uint16_t offset;
    uint8_t length;
    uint8_t value[24];
    uint8_t mask[24];
};

struct gw_profile {
    uint64_t revision;
    uint8_t scan_flags;
    uint16_t interval_ticks;
    uint16_t window_ticks;
    int8_t min_rssi;
    uint8_t uuid_count;
    uint8_t uuids[GW_UUIDS_MAX][16];
    uint8_t company_count;
    uint16_t companies[GW_UUIDS_MAX];
    uint8_t rule_count;
    struct gw_beacon_rule rules[GW_RULES_MAX];
};

void gw_profile_default(struct gw_profile *profile);
/* extended=false parses ScanConfig, true parses ScanProfile. Output changes only on success. */
bool gw_profile_parse(const uint8_t *data, size_t length, bool extended,
                      struct gw_profile *profile);
size_t gw_profile_encode(const struct gw_profile *profile, bool extended,
                         uint8_t *data, size_t capacity);
bool gw_ad_valid(const uint8_t *ad, size_t length);
/* Complete input report, kind 0..3. A disabled profile never matches. */
bool gw_filter_match(const struct gw_profile *profile, uint8_t kind, int8_t rssi,
                     const uint8_t *ad, size_t length);

struct gw_queued_record {
    uint16_t offset;
    uint16_t length;
    uint8_t type;
    uint64_t sequence;
    uint64_t queued_ms;
};

struct gw_queue {
    uint8_t bytes[GW_QUEUE_BYTES];
    struct gw_queued_record records[GW_QUEUE_RECORDS];
    uint16_t head;
    uint16_t count;
    uint16_t write_offset;
    uint32_t used_bytes;
    uint64_t next_sequence;
    uint32_t next_batch;
    uint16_t inflight_count;
    uint32_t inflight_batch;
    bool fault;
};

/* Queue functions require caller serialization, including BLE callback access. */
void gw_queue_init(struct gw_queue *queue);
bool gw_queue_push(struct gw_queue *queue, uint8_t type, const uint8_t *record,
                   size_t length, uint64_t now_ms);
/* Produces/freeze a ready batch, or reserializes an existing one. link_id is caller-owned. */
bool gw_queue_batch(struct gw_queue *queue, uint64_t now_ms, struct gw_frame *frame);
/* Accepts only current batch. CUSTODY pops it, BUSY retains it, FAULT stops automatic batching. */
bool gw_queue_ack(struct gw_queue *queue, uint32_t batch, uint8_t status);

#endif
