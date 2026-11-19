#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialization does not open Wi-Fi. Caller must authorize each window. */
esp_err_t gw_provision_init(void);
esp_err_t gw_provision_open_window(uint32_t seconds);
void gw_provision_close_window(void);
bool gw_provision_is_open(void);
