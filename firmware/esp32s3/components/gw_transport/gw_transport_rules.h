#pragma once
#include "cJSON.h"
#include <stdbool.h>
const char *gw_transport_topic(const char *path,const char **ack);
bool gw_transport_identity(const cJSON *,const char *namespace_id,const char *gateway_id);
bool gw_transport_ack_matches(const cJSON *ack,const cJSON *request,const char *path);
cJSON *gw_transport_parse(const char *bytes,size_t length);
