#include "freertos/FreeRTOS.h"
#include "gw_core.h"
#include "gw_time_bounds.h"
#include <sys/time.h>
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static gw_time_anchor_t anchor;
esp_err_t gw_core_time_accept(uint64_t issued, uint32_t uncertainty,
                              uint64_t sent, uint64_t received) {
  gw_time_anchor_t next;
  if (!gw_time_anchor_set(&next, issued, uncertainty, sent, received))
    return ESP_ERR_INVALID_RESPONSE;
  portENTER_CRITICAL(&lock);
  anchor = next;
  portEXIT_CRITICAL(&lock);
  /* POSIX time is for formatting and SDK TLS; authorization uses bounds. */
  uint64_t middle = next.lower + (next.upper - next.lower) / 2;
  struct timeval tv = {.tv_sec = middle / 1000,
                       .tv_usec = (middle % 1000) * 1000};
  settimeofday(&tv, NULL);
  return ESP_OK;
}
bool gw_core_time_bounds(uint64_t *lower, uint64_t *upper) {
  portENTER_CRITICAL(&lock);
  gw_time_anchor_t current = anchor;
  portEXIT_CRITICAL(&lock);
  return gw_time_anchor_bounds(&current, gw_core_mono_ms(), lower, upper);
}
bool gw_core_time_trusted(void) { return gw_core_time_bounds(NULL, NULL); }
uint64_t gw_core_time_deadline(uint64_t expires_utc_ms) {
  uint64_t upper, now = gw_core_mono_ms();
  if (!gw_core_time_bounds(NULL, &upper) || expires_utc_ms <= upper)
    return now;
  /* Conservative conversion for the verified 100 ppm monotonic error. */
  uint64_t remaining = expires_utc_ms - upper;
  return now + remaining - (remaining + 9999) / 10000;
}
