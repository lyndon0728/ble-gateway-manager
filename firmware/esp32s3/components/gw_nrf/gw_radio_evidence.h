#pragma once
#include "gw_nrf.h"

/* The caller has already read back the entire AdvConfig byte-for-byte. This
 * second proof checks runtime state; enabled alone never proves transmission.
 * NOT_FINISHED asks the caller to obtain live Peripheral state for PAUSED. */
static inline esp_err_t gw_advertising_evidence(const uint8_t *target,
                                                size_t size,
                                                const uint8_t *actual,
                                                size_t length) {
  if (!target || size < 14 || !actual || length != 35)
    return ESP_ERR_INVALID_RESPONSE;
  size_t ad_length = target[12];
  if (ad_length > 31 || size < 14 + ad_length)
    return ESP_ERR_INVALID_RESPONSE;
  size_t response_length = target[13 + ad_length];
  if (response_length > 31 || size != 14 + ad_length + response_length ||
      gw_get_u64(actual) != gw_get_u64(target) || actual[8] != target[8] ||
      actual[10] != target[9] ||
      gw_get_u16(actual + 11) != gw_get_u16(target + 10) ||
      actual[13] != ad_length || actual[14] != response_length ||
      gw_get_u32(actual + 15))
    return ESP_ERR_INVALID_RESPONSE;
  if ((!target[8] && actual[9] == 0) || (target[8] && actual[9] == 1))
    return ESP_OK;
  if (target[8] && target[9] == 2 && actual[9] == 3)
    return ESP_ERR_NOT_FINISHED;
  return ESP_ERR_INVALID_STATE;
}
static inline bool gw_peripheral_live(const uint8_t *state, size_t size) {
  return state && size == 16 && gw_get_u32(state) &&
         (state[4] == 1 || state[4] == 2);
}
