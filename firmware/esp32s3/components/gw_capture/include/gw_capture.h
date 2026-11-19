#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include "gw_wire.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t gw_capture_init(void);
/* UART callback: 0 durable custody, 1 BUSY, 2 FAULT, 255 stale (no ACK). */
uint8_t gw_capture_nrf(const uint8_t boot[16], const struct gw_frame *,
                       void *context);
size_t gw_capture_used_bytes(void);
esp_err_t gw_capture_upload(const char *namespace_id, const char *gateway_id,
                            uint64_t deadline_ms);
/* Complete object: batchMaxRecords 1..1000, flushIntervalMs 100..60000.
 * Validation has no side effects; configure persists before changing runtime.
 */
bool gw_capture_upload_valid(const cJSON *complete_upload);
esp_err_t gw_capture_configure_upload(const cJSON *complete_upload);
cJSON *gw_capture_upload_configuration(void); /* caller owns result */
/* Same complete 24-byte BLE record header as UART, capture_ms in ESP monotonic
 * milliseconds. Call on a worker, never the controller callback. */
esp_err_t gw_capture_esp(const uint8_t *record, size_t length);
esp_err_t gw_capture_esp_gatt(const uint8_t boot[16], const uint8_t *record,
                              size_t length);
void gw_capture_esp_boot(uint8_t boot[16]);
void gw_capture_radio_enabled(uint8_t radio,
                              bool enabled); /* 0 NRF54, 1 ESP32 */
/* Only un-retried TIME_PROBE samples. Capture's worker probes after rehandshake
 * and every 30 seconds; UART command 0x04 must never retransmit. */
esp_err_t gw_capture_time_anchor(const uint8_t boot[16], uint64_t esp_tx_ms,
                                 uint64_t esp_rx_ms, uint64_t nrf_rx_ms,
                                 uint64_t nrf_tx_ms, bool retried);
void gw_capture_connection_ref(uint8_t radio, const uint8_t boot[16],
                               uint32_t conn_id, char out[33]);
bool gw_capture_adv_evidence(const char *event_id, const char *address,
                             const char *address_type,
                             const uint8_t nrf_boot[16], uint32_t max_age_ms);
esp_err_t gw_capture_link_down(uint8_t radio, const uint8_t boot[16],
                               uint32_t conn_id, uint8_t addr_type,
                               const uint8_t address[6], const char *reason);
/* Separate opaque inbox. No automatic command execution or RawEvent mapping.
 * Release only AFTER application authentication/replay checks and independent
 * durable custody. The token binds the immutable observation, not a slot. */
cJSON *gw_capture_peripheral_peek(void);
esp_err_t gw_capture_peripheral_ack(const char *receipt_token);
cJSON *gw_capture_diagnostics(void);
