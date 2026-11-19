#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
/* Public pure verifier for host golden/tamper tests. */
esp_err_t gw_signed_time_verify(const cJSON *response, const char *key_id,
    const uint8_t public_key[32], const char *nonce, const char *boot_id,
    uint64_t *issued_ms, uint32_t *uncertainty_ms);
esp_err_t gw_signed_time_refresh(const char *boot_id);
