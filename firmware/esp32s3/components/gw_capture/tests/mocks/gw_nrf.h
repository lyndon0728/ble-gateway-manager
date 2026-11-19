#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
bool gw_nrf_identity(uint8_t *,uint32_t *,char *);
esp_err_t gw_nrf_command(uint8_t,const void *,size_t,uint8_t *,void *,size_t *,int64_t);
