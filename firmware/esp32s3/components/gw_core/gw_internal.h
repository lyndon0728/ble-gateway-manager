#pragma once
#include "gw_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

esp_err_t gw_store_init(void);
esp_err_t gw_network_init(void);
esp_err_t gw_network_validate(const cJSON *network, char *error, size_t size);
esp_err_t gw_network_apply(const cJSON *network);
esp_err_t gw_network_scan(bool active, uint64_t deadline_mono_ms, cJSON **result);
esp_err_t gw_credential_password(const char *ref, const char *ssid,
                               const char *security, char password[65]);
bool gw_json_u64(const cJSON *v, uint64_t *out);
bool gw_json_allowed(const cJSON *object, const char *const *keys, size_t n);
void gw_json_set_string(cJSON *o, const char *key, const char *value);
void gw_json_set_u64(cJSON *o, const char *key, uint64_t value);
const char *gw_json_string(const cJSON *o, const char *key);
char *gw_json_canonical(const cJSON *value); /* caller frees; bounded depth */
/* Secret blobs are read on demand, never cached in the plaintext KV snapshot. */
esp_err_t gw_secret_get(const char *key, void *data, size_t *size);
esp_err_t gw_secret_put(const char *key, const void *data, size_t size);
