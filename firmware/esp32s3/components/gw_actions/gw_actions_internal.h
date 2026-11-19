#pragma once
#include "gw_actions.h"
#include "gw_core.h"
#include "gw_wire.h"

const char *gw_action_string(const cJSON *o, const char *key);
bool gw_action_hex(const char *s, uint8_t *out, size_t n);
void gw_action_hex_encode(const uint8_t *in, size_t n, char *out);
bool gw_action_uuid(const char *s, uint8_t out[16]);
esp_err_t gw_action_profile(const cJSON *complete, uint64_t revision, struct gw_profile *profile);
esp_err_t gw_ble_init(void);
esp_err_t gw_ble_configure(const cJSON *complete, bool advertising, uint64_t revision, uint64_t deadline, cJSON **details);
esp_err_t gw_ble_execute(const cJSON *command, uint64_t deadline, cJSON **details);
cJSON *gw_ble_diagnostics(void);
esp_err_t gw_ble_pause(uint64_t deadline);
esp_err_t gw_ble_restore(uint64_t deadline);
esp_err_t gw_gatt_nrf_init(void);
esp_err_t gw_gatt_nrf_execute(const cJSON *command, uint64_t deadline, cJSON **details);
esp_err_t gw_gatt_nrf_recover(const cJSON *command, uint64_t deadline, cJSON **details);
esp_err_t gw_ota_init(void);
esp_err_t gw_ota_execute(const cJSON *command, uint64_t deadline, cJSON **details);
esp_err_t gw_ota_recover(const cJSON *command, uint64_t deadline, cJSON **details);
