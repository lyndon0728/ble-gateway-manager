#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Bounds/UTF-8 check before the allocating JSON parser. Not a JSON parser. */
bool gw_prov_payload_precheck(const uint8_t *data,size_t size);
