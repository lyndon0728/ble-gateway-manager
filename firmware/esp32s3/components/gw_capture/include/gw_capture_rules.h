#pragma once
#include "gw_spool.h"
/* Pure C protocol checks and pair predicate, shared by firmware and host tests. */
bool gw_capture_record_valid(uint8_t type,const uint8_t *record,size_t length);
bool gw_capture_pairable(const gw_spool_record_t *,const gw_spool_record_t *,uint64_t *delta_ms);
bool gw_capture_map_time(uint64_t capture_ms,uint64_t anchor_nrf_ms,uint64_t anchor_esp_ms,
    uint32_t anchor_error_ms,uint64_t *mapped_ms,uint32_t *error_ms);
