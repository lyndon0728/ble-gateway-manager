#ifndef GW_FILTER_V2_H
#define GW_FILTER_V2_H
#include "gw_core.h"

#define GW_FILTER_V2_MAX 3070U
#define GW_FILTER_V2_PENDING 32U
#define GW_FILTER_V2_STATUS_SIZE 96U
struct gw_filter_v2_rule {
    uint8_t action, kind_mask;
    uint16_t field_mask;
    uint8_t name_mode, name_source, name_length;
    uint8_t name_value[29], name_mask[29];
    uint8_t ibeacon_value[16], ibeacon_mask[16];
    uint8_t rssi_mode, rssi_arg0, rssi_arg1;
    uint16_t major_value, major_mask, minor_value, minor_mask;
    uint8_t service_value[16], service_mask[16];
};
struct gw_filter_v2_config {
    struct gw_profile legacy;
    uint8_t engine, scope, rule_count;
    uint16_t pair_window_ms;
    struct gw_filter_v2_rule rules[GW_RULES_MAX];
};
enum gw_filter_v2_counter {
    GW_FILTER_SEEN, GW_FILTER_PASSED, GW_FILTER_RULE_DROPPED,
    GW_FILTER_PAIR_TIMEOUT, GW_FILTER_PAIR_AMBIGUOUS, GW_FILTER_PAIR_OVERFLOW,
    GW_FILTER_MALFORMED_AD, GW_FILTER_UNKNOWN_RSSI,
    GW_FILTER_UNSUPPORTED_CONTEXT, GW_FILTER_CONFIG_DISCARDED,
    GW_FILTER_COUNTER_COUNT
};
struct gw_filter_v2_pending { uint8_t length; uint8_t record[55]; };
struct gw_filter_v2_state {
    uint64_t counters[GW_FILTER_COUNTER_COUNT];
    struct gw_filter_v2_pending pending[GW_FILTER_V2_PENDING];
};
typedef void (*gw_filter_v2_emit)(void *user, const uint8_t *record, size_t length);
void gw_filter_v2_default(struct gw_filter_v2_config *config);
/* Parse changes output only on success; encode returns zero for invalid configs. */
bool gw_filter_v2_parse(const uint8_t *data, size_t length, struct gw_filter_v2_config *config);
size_t gw_filter_v2_encode(const struct gw_filter_v2_config *config, uint8_t *data, size_t capacity);
/* All state operations require caller serialization. Config is an immutable snapshot
 * for a feed. Clear before replacing it or changing controller identity/scan state. */
void gw_filter_v2_feed(struct gw_filter_v2_state *state, const struct gw_filter_v2_config *config,
                       const uint8_t *record, size_t length, gw_filter_v2_emit emit, void *user);
/* Advance only after all input timestamps <= watermark have been processed/published.
 * Exact deadline is inclusive; only deadline < watermark expires. */
void gw_filter_v2_advance(struct gw_filter_v2_state *state, uint64_t watermark_ms);
void gw_filter_v2_clear(struct gw_filter_v2_state *state);
void gw_filter_v2_status(const struct gw_filter_v2_state *state,
                         const struct gw_filter_v2_config *config, uint8_t out[96]);
#endif
