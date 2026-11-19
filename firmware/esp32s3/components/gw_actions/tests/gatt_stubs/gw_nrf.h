#pragma once
#include "gw_core.h"
bool gw_nrf_identity(uint8_t[16],uint32_t *,char[64]);
esp_err_t gw_nrf_command(uint8_t,const void *,size_t,uint8_t *,void *,size_t *,int64_t);
uint16_t gw_get_u16(const uint8_t *);
uint32_t gw_get_u32(const uint8_t *);
uint64_t gw_get_u64(const uint8_t *);
void gw_put_u16(uint8_t *,uint16_t);
void gw_put_u32(uint8_t *,uint32_t);
void gw_put_u64(uint8_t *,uint64_t);
