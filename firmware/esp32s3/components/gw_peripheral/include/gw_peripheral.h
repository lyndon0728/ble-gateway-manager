#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdint.h>
esp_err_t gw_peripheral_init(void);
esp_err_t gw_peripheral_service(const char *namespace_id,
                                const char *gateway_id, const char *interface,
                                uint64_t deadline_ms);
esp_err_t gw_peripheral_execute(const cJSON *command, uint64_t deadline_ms,
                                cJSON **details);
/* Read durable results only. Ambiguous delivery is never automatically
 * repeated. */
esp_err_t gw_peripheral_recover(const cJSON *command, uint64_t deadline_ms,
                                cJSON **details);
cJSON *gw_peripheral_diagnostics(void);
