#pragma once
#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
uint64_t gw_core_mono_ms(void);
bool gw_core_time_trusted(void);
esp_netif_t *gw_core_uplink_netif(const char *);
esp_err_t gw_factory_get_string(const char *, char *, size_t);
