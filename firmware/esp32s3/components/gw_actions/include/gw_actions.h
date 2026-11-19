#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/* Initialize radio ownership and recover durable action/OTA evidence before
 * accepting cloud commands. Deadlines below are absolute monotonic millis. */
esp_err_t gw_actions_init(void);
esp_err_t gw_actions_configure(const cJSON *patch, const cJSON *complete,
                              uint64_t revision, uint64_t deadline_ms,
                              cJSON **details);
esp_err_t gw_actions_execute(const cJSON *command, uint64_t deadline_ms,
                            cJSON **details);
/* Recover facts only; never repeats an ambiguous physical action. NOT_FINISHED
 * means an outstanding trial/maintenance obligation, TIMEOUT means UNKNOWN. */
esp_err_t gw_actions_recover(const cJSON *command, uint64_t deadline_ms,
                            cJSON **details);
