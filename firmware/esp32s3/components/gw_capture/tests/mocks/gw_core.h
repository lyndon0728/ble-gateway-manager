#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
bool gw_core_time_trusted(void);
cJSON *gw_store_get_json(const char *);
esp_err_t gw_store_get(const char *, void *, size_t *);
esp_err_t gw_store_put_json(const char *, const cJSON *);
