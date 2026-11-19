#pragma once
#include "gw_core.h"
void gw_capture_connection_ref(uint8_t,const uint8_t[16],uint32_t,char[33]);
bool gw_capture_adv_evidence(const char *,const char *,const char *,const uint8_t[16],uint32_t);
esp_err_t gw_capture_link_down(uint8_t,const uint8_t[16],uint32_t,uint8_t,const uint8_t[6],const char *);
