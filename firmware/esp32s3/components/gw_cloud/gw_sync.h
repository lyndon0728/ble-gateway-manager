#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdint.h>
void gw_cloud_sync(const char *namespace_id,const char *gateway_id,const char *esp_boot_id);
esp_err_t gw_cloud_snapshot_check(const cJSON *snapshot,const char *namespace_id,const char *gateway_id);
esp_err_t gw_cloud_recovery_check(const cJSON *command,const char *namespace_id,const char *gateway_id,uint64_t deadline);
