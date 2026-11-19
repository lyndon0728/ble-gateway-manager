#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
void gw_capture_radio_enabled(uint8_t,bool);
void gw_capture_esp_boot(uint8_t[16]);
bool gw_capture_upload_valid(const cJSON *);
esp_err_t gw_capture_configure_upload(const cJSON *);
cJSON *gw_capture_upload_configuration(void);
