#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint64_t lower, upper, received, expires; bool valid; } gw_time_anchor_t;
/* Inputs are accepted only after signature, nonce, boot and schema validation. */
bool gw_time_anchor_set(gw_time_anchor_t *a, uint64_t issued, uint32_t uncertainty,
                        uint64_t sent, uint64_t received);
bool gw_time_anchor_bounds(const gw_time_anchor_t *a, uint64_t now,
                           uint64_t *lower, uint64_t *upper);
