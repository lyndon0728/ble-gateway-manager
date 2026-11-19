#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include <stdint.h>
esp_err_t gw_http_json(const char *,const char *,const cJSON *,const char *,uint64_t,cJSON **);
