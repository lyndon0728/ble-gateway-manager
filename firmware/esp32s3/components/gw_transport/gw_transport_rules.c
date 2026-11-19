#include "gw_transport_rules.h"
#include <string.h>
static bool unique_json(const cJSON *j, unsigned depth) {
  if (depth > 16)
    return false;
  for (const cJSON *v = j ? j->child : NULL; v; v = v->next) {
    if (cJSON_IsObject(j))
      for (const cJSON *n = v->next; n; n = n->next)
        if (!strcmp(v->string, n->string))
          return false;
    if (!unique_json(v, depth + 1))
      return false;
  }
  return true;
}
cJSON *gw_transport_parse(const char *bytes, size_t length) {
  if (!bytes || !length || length > 128u * 1024u || memchr(bytes, 0, length))
    return NULL;
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < length; i++) {
    char c = bytes[i];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\') {
        if (i + 5 < length && !memcmp(bytes + i + 1, "u0000", 5))
          return NULL;
        escaped = true;
      } else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 16)
        return NULL;
    } else if (c == '}' || c == ']') {
      if (!depth)
        return NULL;
      --depth;
    }
  }
  if (quoted || depth)
    return NULL;
  const char *end = NULL;
  cJSON *j = cJSON_ParseWithLengthOpts(bytes, length + 1, &end, true);
  if (!unique_json(j, 0)) {
    cJSON_Delete(j);
    return NULL;
  }
  return j;
}
static const char *s(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
const char *gw_transport_topic(const char *path, const char **ack) {
  *ack = NULL;
  if (!strcmp(path, "/api/v1/gateway-ingress/batches")) {
    *ack = "telemetry-ack";
    return "telemetry";
  }
  if (!strcmp(path, "/api/v1/gateway-ingress/results")) {
    *ack = "result-ack";
    return "command-results";
  }
  if (!strcmp(path, "/api/v1/gateway-peripheral/batches")) {
    *ack = "peripheral-ack";
    return "peripheral-events";
  }
  if (!strcmp(path, "/api/v1/gateway-ingress/status"))
    return "status";
  if (!strcmp(path, "/api/v1/gateway-ingress/commands"))
    return "commands";
  return NULL;
}
bool gw_transport_identity(const cJSON *j, const char *ns, const char *gw) {
  return cJSON_IsObject(j) && !strcmp(s(j, "namespaceId"), ns) &&
         !strcmp(s(j, "gatewayId"), gw);
}
bool gw_transport_ack_matches(const cJSON *ack, const cJSON *request,
                              const char *path) {
  if (!gw_transport_identity(ack, s(request, "namespaceId"),
                             s(request, "gatewayId")))
    return false;
  if (!strcmp(path, "/api/v1/gateway-ingress/results")) {
    if (strcmp(s(ack, "durability"), "RESULT_PERSISTED"))
      return false;
    const cJSON *r = cJSON_GetArrayItem(
                    cJSON_GetObjectItemCaseSensitive(request, "results"), 0),
                *item;
    if (!*s(r, "resultId"))
      return false;
    cJSON_ArrayForEach(
        item,
        cJSON_GetObjectItemCaseSensitive(
            ack, "items")) if (!strcmp(s(item, "resultId"), s(r, "resultId")) &&
                               !strcmp(s(item, "taskId"),
                                       s(r, "taskId"))) return true;
    return false;
  }
  const char *durability = !strcmp(path, "/api/v1/gateway-peripheral/batches")
                               ? "PERIPHERAL_PERSISTED"
                               : "RAW_PERSISTED";
  return *s(request, "batchId") &&
         !strcmp(s(ack, "batchId"), s(request, "batchId")) &&
         !strcmp(s(ack, "durability"), durability);
}
