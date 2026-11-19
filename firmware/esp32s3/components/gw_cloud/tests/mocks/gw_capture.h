#pragma once
#include <stddef.h>
#include <stdint.h>
size_t gw_capture_used_bytes(void);
void gw_capture_upload(const char *, const char *, uint64_t);
void gw_capture_esp_boot(uint8_t boot[16]);
