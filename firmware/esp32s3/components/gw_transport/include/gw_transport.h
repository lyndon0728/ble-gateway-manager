#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdint.h>
esp_err_t gw_transport_init(const char *namespace_id,const char *gateway_id);
void gw_transport_service(void);
/* One task journal/spool and one downlink consumer for both transports.
 * Named-interface candidate proofs always use bound HTTPS. */
esp_err_t gw_transport_json(const char *path,const char *method,const cJSON *body,
    const char *interface,uint64_t deadline,cJSON **response);
