#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#include "gw_wire.h"

/* Receiver returns protocol ACK: 0 durable custody, 1 temporary BUSY,
 * 2 storage/sequence FAULT, 255 stale batch silently ignored.
 * Receiving into RAM is NEVER durable custody. */
typedef uint8_t (*gw_nrf_batch_fn)(const uint8_t boot[16], const struct gw_frame *, void *);
esp_err_t gw_nrf_init(gw_nrf_batch_fn receive, void *context);
bool gw_nrf_identity(uint8_t boot[16], uint32_t *capabilities, char version[64]);
esp_err_t gw_nrf_rehandshake(int64_t deadline_ms);
/* deadline is absolute ESP monotonic milliseconds; status is the peer status.
 * A timeout is an UNKNOWN execution outcome, never permission to repeat an action. */
esp_err_t gw_nrf_command(uint8_t opcode, const void *args, size_t size,
                        uint8_t *status, void *result, size_t *result_size,
                        int64_t deadline_ms);
esp_err_t gw_nrf_scan(const cJSON *complete, uint64_t revision, int64_t deadline_ms);
esp_err_t gw_nrf_advertise(const cJSON *complete, uint64_t revision, int64_t deadline_ms);
cJSON *gw_nrf_diagnostics(int64_t deadline_ms);
int64_t gw_nrf_now_ms(void);
