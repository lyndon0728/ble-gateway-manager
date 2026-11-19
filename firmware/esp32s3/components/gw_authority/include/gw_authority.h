#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Read/verify immutable manufacturing evidence once before worker startup. */
esp_err_t gw_authority_init(void);
bool gw_authority_has(const char *capability);
bool gw_authority_board_verified(const char *capability);
const char *gw_authority_digest(void);
cJSON *gw_authority_factory_image(const char *component); /* signed candidate, caller owns */
/* Pure verifier: returned authorized capability array is caller owned. */
esp_err_t gw_authority_verify(const char *manifest,const char *artifact,
    const uint8_t signature[64],const uint8_t public_key[32],const char *key_id,
    const char *gateway_id,const char *sn,const char *model,const char *hardware,
    cJSON **capabilities,char digest[65]);
