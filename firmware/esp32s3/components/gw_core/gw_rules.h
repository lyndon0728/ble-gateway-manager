#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool gw_rules_u64(const char *text, uint64_t *value);
bool gw_rules_utf8(const char *text);
bool gw_rules_ipv4(const char *text, uint32_t *value);
uint32_t gw_record_crc32(const void *data, size_t size);
void gw_record_put32(uint8_t *out,uint32_t value);
uint32_t gw_record_get32(const uint8_t *in);
void gw_record_put64(uint8_t *out,uint64_t value);
uint64_t gw_record_get64(const uint8_t *in);
bool gw_record_valid_snapshot(const uint8_t *record,size_t size,uint64_t generation,uint32_t payload_crc);
bool gw_record_valid_pointer(const uint8_t *record,size_t size);
