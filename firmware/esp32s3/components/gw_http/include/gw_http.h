#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
/* Always validates TLS certificate/name, disables redirects, and uses device
 * credentials only with the factory origin. A named interface binds DNS AND TLS. */
esp_err_t gw_http_json(const char *path, const char *method, const cJSON *body,
                      const char *interface, uint64_t deadline_ms, cJSON **response);
typedef esp_err_t (*gw_http_sink_fn)(const uint8_t *data, size_t n, void *context);
esp_err_t gw_http_stream(const char *path, uint64_t deadline_ms, size_t limit,
                        gw_http_sink_fn sink, void *context, size_t *received);
