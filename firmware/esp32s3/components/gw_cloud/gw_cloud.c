#include "gw_cloud.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_actions.h"
#include "gw_capture.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_nrf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JOURNAL_KEY "cloud/journal"
#define MAX_TASKS 64
static SemaphoreHandle_t mutex;
static cJSON *journal;
static char namespace_id[129], gateway_id[129], boot_id[33];
static bool identity_ready;
static const char *str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static double num(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
static void put(cJSON *o, const char *key, cJSON *v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, key);
  cJSON_AddItemToObject(o, key, v);
}
static void text(cJSON *o, const char *key, const char *v) {
  put(o, key, cJSON_CreateString(v));
}
static void random_id(char *out, size_t bytes) {
  uint8_t raw[32];
  if (bytes > 32)
    bytes = 32;
  esp_fill_random(raw, bytes);
  for (size_t i = 0; i < bytes; i++)
    sprintf(out + 2 * i, "%02x", raw[i]);
  out[2 * bytes] = 0;
}
static bool valid_id(const char *s) {
  if (!s || !s[0] || strlen(s) > 128)
    return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
          (*s >= '0' && *s <= '9') || strchr("_.:-", *s)))
      return false;
  return true;
}
static bool utc(char out[32]) {
  if (!gw_core_time_trusted())
    return false;
  time_t now = time(NULL);
  struct tm tm;
  if (!gmtime_r(&now, &tm))
    return false;
  return strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm) > 0;
}
static int64_t parse_utc(const char *s) {
  int y, m, d, h, mi, se, used = 0;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &m, &d, &h, &mi, &se, &used) !=
          6 ||
      used != 19)
    return -1;
  static const unsigned md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 ||
      d > (int)(md[m - 1] +
                (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) ||
      h > 23 || mi > 59 || se > 59 || h < 0 || mi < 0 || se < 0)
    return -1;
  const char *p = s + 19;
  if (*p == '.') {
    p++;
    if (*p < '0' || *p > '9')
      return -1;
    while (*p >= '0' && *p <= '9')
      p++;
  }
  int zone = 0;
  if (*p == 'Z' && !p[1]) {
  } else if ((*p == '+' || *p == '-') && strlen(p) == 6) {
    int zh, zm;
    if (sscanf(p + 1, "%2d:%2d", &zh, &zm) != 2 || zh > 23 || zm > 59)
      return -1;
    zone = (zh * 60 + zm) * 60 * (*p == '+' ? 1 : -1);
  } else
    return -1;
  y -= m <= 2;
  int era = y / 400;
  unsigned yo = y - era * 400, mp = m + (m > 2 ? -3 : 9),
           day = (153 * mp + 2) / 5 + d - 1,
           doe = yo * 365 + yo / 4 - yo / 100 + day;
  return ((int64_t)era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 +
         se - zone;
}
static cJSON *records(void) {
  return cJSON_GetObjectItemCaseSensitive(journal, "records");
}
static cJSON *find_record(const char *id) {
  cJSON *r;
  cJSON_ArrayForEach(r, records()) if (!strcmp(str(r, "taskId"), id)) return r;
  return NULL;
}
static esp_err_t save(void) {
  esp_err_t e = gw_store_put_json(JOURNAL_KEY, journal);
  if (e != ESP_OK) {
    cJSON_Delete(journal);
    journal = gw_store_get_json(JOURNAL_KEY);
  }
  return e;
}
static cJSON *envelope(const char *collection, cJSON *value) {
  cJSON *o = cJSON_CreateObject();
  if (!o) {
    cJSON_Delete(value);
    return NULL;
  }
  cJSON_AddStringToObject(o, "schemaVersion", "1.0");
  cJSON_AddStringToObject(o, "namespaceId", namespace_id);
  cJSON_AddStringToObject(o, "gatewayId", gateway_id);
  cJSON_AddItemToObject(o, collection, value);
  return o;
}
static bool ack_result(const cJSON *ack, const char *task, const char *id) {
  if (strcmp(str(ack, "namespaceId"), namespace_id) ||
      strcmp(str(ack, "gatewayId"), gateway_id) ||
      strcmp(str(ack, "durability"), "RESULT_PERSISTED"))
    return false;
  const cJSON *v;
  cJSON_ArrayForEach(
      v, cJSON_GetObjectItemCaseSensitive(
             ack, "items")) if (!strcmp(str(v, "taskId"), task) &&
                                !strcmp(str(v, "resultId"), id) &&
                                (!strcmp(str(v, "status"), "ACCEPTED") ||
                                 !strcmp(str(v, "status"),
                                         "DUPLICATE"))) return true;
  return false;
}
static esp_err_t deliver(const cJSON *result, const char *interface,
                         uint64_t deadline) {
  cJSON *array = cJSON_CreateArray();
  cJSON_AddItemToArray(array, cJSON_Duplicate(result, true));
  cJSON *batch = envelope("results", array), *ack = NULL;
  if (!batch)
    return ESP_ERR_NO_MEM;
  esp_err_t e = gw_http_json("/api/v1/gateway-ingress/results", "POST", batch,
                             interface, deadline, &ack);
  if (e == ESP_OK &&
      !ack_result(ack, str(result, "taskId"), str(result, "resultId")))
    e = ESP_ERR_INVALID_RESPONSE;
  cJSON_Delete(batch);
  cJSON_Delete(ack);
  if (e == ESP_OK) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    cJSON *r = find_record(str(result, "taskId"));
    if (r) {
      cJSON *pending = cJSON_GetObjectItemCaseSensitive(r, "pending"), *v;
      int index = 0;
      cJSON_ArrayForEach(v, pending) {
        if (!strcmp(str(v, "resultId"), str(result, "resultId"))) {
          cJSON_DeleteItemFromArray(pending, index);
          break;
        }
        index++;
      }
      if (!strcmp(str(result, "state"), "SUCCEEDED")) {
        text(r, "successAck", "YES");
        const cJSON *details =
            cJSON_GetObjectItemCaseSensitive(result, "details");
        const cJSON *rev = cJSON_GetObjectItemCaseSensitive(
            details, "reportedConfigurationRevision");
        if (cJSON_IsNumber(rev))
          put(journal, "reportedRevision", cJSON_Duplicate(rev, true));
      }
      const char *stage = str(result, "state");
      if (strcmp(stage, "ACCEPTED") && strcmp(stage, "EXECUTING")) {
        /* Payloads acknowledged by the platform may be compacted; the
         * immutable terminal result and task digest remain for replay. */
        const char *drop[] = {"reserve", "command", "candidate",
                              "networkStart"};
        for (size_t i = 0; i < sizeof(drop) / sizeof(*drop); i++)
          cJSON_DeleteItemFromObjectCaseSensitive(r, drop[i]);
        cJSON *final = cJSON_CreateObject();
        cJSON_AddItemToObject(final, "terminal", cJSON_Duplicate(result, true));
        put(r, "phases", final);
        text(r, "phase", "DONE");
      }
      e = save();
    }
    xSemaphoreGive(mutex);
    if (e == ESP_OK && !strcmp(str(result, "state"), "SUCCEEDED"))
      (void)gw_core_platform_ack(str(result, "taskId"));
  }
  return e;
}
/* Stable phase keys retain result identity through retries, including probe ACK
 * received just before a power cut. No result is sent before its journal
 * commit. */
static esp_err_t result_for(const char *id, const char *phase,
                            const char *state, const char *verification,
                            const cJSON *details, const char *error,
                            const char *interface, uint64_t deadline) {
  char observed[32];
  if (!utc(observed))
    return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *r = find_record(id);
  cJSON *result = NULL;
  esp_err_t e = ESP_ERR_INVALID_STATE;
  if (!r)
    goto unlock;
  cJSON *phases = cJSON_GetObjectItemCaseSensitive(r, "phases");
  const cJSON *existing = cJSON_GetObjectItemCaseSensitive(phases, phase);
  if (existing) {
    result = cJSON_Duplicate(existing, true);
    e = result ? ESP_OK : ESP_ERR_NO_MEM;
    goto unlock;
  }
  if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "pending")) >= 8) {
    e = ESP_ERR_NO_MEM;
    goto unlock;
  }
  result = cJSON_CreateObject();
  char rid[33];
  random_id(rid, 16);
  if (!result) {
    e = ESP_ERR_NO_MEM;
    goto unlock;
  }
  cJSON_AddStringToObject(result, "resultId", rid);
  cJSON_AddStringToObject(result, "taskId", id);
  double seq = num(r, "nextSequence");
  if (seq > 9007199254740990.0) {
    e = ESP_ERR_INVALID_SIZE;
    goto unlock;
  }
  cJSON_AddNumberToObject(result, "resultSequence", seq);
  put(r, "nextSequence", cJSON_CreateNumber(seq + 1));
  cJSON_AddStringToObject(result, "state", state);
  cJSON_AddStringToObject(result, "observedAt", observed);
  cJSON_AddStringToObject(result, "verification", verification);
  cJSON_AddItemToObject(result, "details",
                        details ? cJSON_Duplicate(details, true)
                                : cJSON_CreateObject());
  if (error && error[0])
    cJSON_AddStringToObject(result, "errorCode", error);
  char *encoded = cJSON_PrintUnformatted(result);
  size_t cost = encoded ? 2 * strlen(encoded) + 512 : SIZE_MAX;
  free(encoded);
  cJSON *reserved = cJSON_GetObjectItemCaseSensitive(r, "reserve");
  size_t available =
      cJSON_IsString(reserved) ? strlen(reserved->valuestring) : 0;
  if (cost > available) {
    e = ESP_ERR_NO_MEM;
    goto unlock;
  }
  reserved->valuestring[available - cost] = 0;
  cJSON_AddItemToObject(phases, phase, cJSON_Duplicate(result, true));
  cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(r, "pending"),
                       cJSON_Duplicate(result, true));
  text(r, "state", state);
  e = save();
unlock:
  xSemaphoreGive(mutex);
  if (e == ESP_OK)
    e = deliver(result, interface, deadline);
  cJSON_Delete(result);
  return e;
}
static esp_err_t probe(const gw_network_probe_t *p, void *unused) {
  (void)unused;
  if (p->remote) {
    cJSON *d = cJSON_CreateObject(),
          *proof = cJSON_AddObjectToObject(d, "networkProbe");
    cJSON_AddStringToObject(proof, "interface", p->interface);
    cJSON_AddNumberToObject(proof, "candidateRevision",
                            strtod(p->desired_revision, NULL));
    cJSON_AddBoolToObject(proof, "linkReady", true);
    cJSON_AddBoolToObject(proof, "ipReady", true);
    /* HTTP helper must successfully complete interface-bound DNS + trusted
     * TLS + authenticated platform persistence before this returns OK. */
    cJSON_AddBoolToObject(proof, "dnsReady", true);
    cJSON_AddBoolToObject(proof, "tlsReady", true);
    char phase[40];
    snprintf(phase, sizeof(phase), "probe_%s", p->interface);
    esp_err_t e = result_for(p->task_id, phase, "EXECUTING", "NONE", d, NULL,
                             p->interface, p->deadline_mono_ms);
    cJSON_Delete(d);
    return e;
  }
  char nonce[33];
  random_id(nonce, 16);
  cJSON *q = cJSON_CreateObject(), *r = NULL;
  cJSON_AddStringToObject(q, "transactionId", p->transaction_id);
  cJSON_AddStringToObject(q, "interface", p->interface);
  cJSON_AddStringToObject(q, "snapshotId", p->snapshot_id);
  cJSON_AddStringToObject(q, "nonce", nonce);
  esp_err_t e = gw_http_json("/api/v1/gateway-network-probes", "POST", q,
                             p->interface, p->deadline_mono_ms, &r);
  if (e == ESP_OK &&
      (strcmp(str(r, "transactionId"), p->transaction_id) ||
       strcmp(str(r, "interface"), p->interface) ||
       strcmp(str(r, "snapshotId"), p->snapshot_id) ||
       strcmp(str(r, "nonce"), nonce) || !str(r, "receiptId")[0]))
    e = ESP_ERR_INVALID_RESPONSE;
  cJSON_Delete(q);
  cJSON_Delete(r);
  return e;
}
static void merge(cJSON *dst, const cJSON *patch) {
  const cJSON *v;
  cJSON_ArrayForEach(v, patch) {
    cJSON *old = cJSON_GetObjectItemCaseSensitive(dst, v->string);
    if (cJSON_IsObject(v) && cJSON_IsObject(old) && strcmp(v->string, "ipv4"))
      merge(old, v);
    else
      put(dst, v->string, cJSON_Duplicate(v, true));
  }
}
static cJSON *default_radio(void) {
  return cJSON_Parse(
      "{\"scan\":{\"enabled\":false,\"active\":false,\"intervalMs\":100,"
      "\"windowMs\":50},\"filters\":{\"serviceUuids\":[],\"manufacturerIds\":[]"
      "},\"beaconRules\":[],\"advertising\":{\"enabled\":false,\"mode\":"
      "\"NONCONNECTABLE\",\"intervalMs\":100,\"advDataBase64\":\"\","
      "\"scanResponseBase64\":\"\"}}");
}
static cJSON *configuration(void) {
  cJSON *cfg = gw_store_get_json("cloud/configuration");
  if (cfg) {
    if (!cJSON_GetObjectItemCaseSensitive(cfg, "upload"))
      cJSON_AddItemToObject(
          cfg, "upload",
          cJSON_Parse("{\"batchMaxRecords\":64,\"flushIntervalMs\":250}"));
    return cfg;
  }
  cfg = cJSON_CreateObject();
  cJSON_AddItemToObject(
      cfg, "upload",
      cJSON_Parse("{\"batchMaxRecords\":64,\"flushIntervalMs\":250}"));
  cJSON *radios = cJSON_AddObjectToObject(cfg, "radios");
  cJSON_AddItemToObject(radios, "nrf54", default_radio());
  cJSON_AddItemToObject(radios, "esp32", default_radio());
  cJSON *state = gw_core_state();
  const cJSON *network =
      cJSON_GetObjectItemCaseSensitive(state, "desiredLocalNetwork");
  if (cJSON_IsObject(network))
    cJSON_AddItemToObject(cfg, "network", cJSON_Duplicate(network, true));
  cJSON_Delete(state);
  return cfg;
}
static void merge_configuration(cJSON *cfg, const cJSON *patch) {
  merge(cfg, patch);
  /* Legacy scan/filters and the explicit NRF54 group share one effective
   * snapshot; switching API versions must not restore stale radio settings. */
  cJSON *nrf = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(cfg, "radios"), "nrf54");
  const char *legacy[] = {"scan", "filters"};
  for (size_t i = 0; i < sizeof(legacy) / sizeof(*legacy); ++i) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(patch, legacy[i]);
    if (value) {
      cJSON *old = cJSON_GetObjectItemCaseSensitive(nrf, legacy[i]);
      if (cJSON_IsObject(value) && cJSON_IsObject(old))
        merge(old, value);
      else
        put(nrf, legacy[i], cJSON_Duplicate(value, true));
      cJSON_DeleteItemFromObjectCaseSensitive(cfg, legacy[i]);
    }
  }
}
static esp_err_t execute_config(const cJSON *command, uint64_t deadline,
                                cJSON **details, bool *async) {
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "parameters"),
              *patch =
                  cJSON_GetObjectItemCaseSensitive(params, "configuration");
  cJSON *cfg = configuration();
  if (!cfg || !cJSON_IsObject(patch)) {
    cJSON_Delete(cfg);
    return ESP_ERR_INVALID_ARG;
  }
  merge_configuration(cfg, patch);
  double revision = num(params, "desiredRevision");
  if (revision < 1 || revision > 9007199254740991.0 ||
      floor(revision) != revision) {
    cJSON_Delete(cfg);
    return ESP_ERR_INVALID_ARG;
  }
  const cJSON *network = cJSON_GetObjectItemCaseSensitive(patch, "network");
  esp_err_t e;
  if (network) {
    /* Reject cross-group mutations before any physical change. */
    if (cJSON_GetArraySize(patch) != 1) {
      cJSON_Delete(cfg);
      return ESP_ERR_INVALID_ARG;
    }
    char rev[24];
    snprintf(rev, sizeof(rev), "%.0f", revision);
    cJSON *response = NULL;
    e = gw_core_remote_begin(
        str(command, "taskId"),
        cJSON_GetObjectItemCaseSensitive(cfg, "network"), rev,
        cJSON_GetObjectItemCaseSensitive(params, "networkPrecondition"),
        deadline, &response);
    if (e == ESP_OK) {
      xSemaphoreTake(mutex, portMAX_DELAY);
      cJSON *r = find_record(str(command, "taskId"));
      if (r) {
        text(r, "phase", "NETWORK");
        put(r, "candidate", cfg);
        cfg = NULL;
        put(r, "networkStart", response);
        response = NULL;
        e = save();
      }
      xSemaphoreGive(mutex);
      *async = e == ESP_OK;
    }
    cJSON_Delete(response);
  } else {
    e = gw_actions_configure(patch, cfg, (uint64_t)revision, deadline, details);
    /* The action target, DONE evidence and complete cloud snapshot are
     * published by one gw_store_transaction in gw_actions_configure. */
  }
  cJSON_Delete(cfg);
  return e;
}
static void finish_config(const cJSON *command, const cJSON *details,
                          uint64_t deadline) {
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "parameters");
  cJSON *d = details ? cJSON_Duplicate(details, true) : cJSON_CreateObject();
  put(d, "configuration",
      cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(params, "configuration"),
                      true));
  cJSON_AddNumberToObject(d, "reportedConfigurationRevision",
                          num(params, "desiredRevision"));
  if (result_for(str(command, "taskId"), "terminal", "SUCCEEDED", "VERIFIED", d,
                 NULL, NULL, deadline) == ESP_OK) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    put(journal, "reportedRevision",
        cJSON_CreateNumber(num(params, "desiredRevision")));
    save();
    xSemaphoreGive(mutex);
  }
  cJSON_Delete(d);
}
static bool command_valid(const cJSON *c) {
  if (!cJSON_IsObject(c) || strcmp(str(c, "schemaVersion"), "1.0") ||
      strcmp(str(c, "namespaceId"), namespace_id) ||
      strcmp(str(c, "gatewayId"), gateway_id) || !valid_id(str(c, "taskId")) ||
      strlen(str(c, "requestDigest")) != 64 ||
      !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(c, "parameters")))
    return false;
  const char *digest = str(c, "requestDigest");
  for (unsigned i = 0; i < 64; i++)
    if (!((digest[i] >= 'a' && digest[i] <= 'f') ||
          (digest[i] >= '0' && digest[i] <= '9')))
      return false;
  const cJSON *field;
  const char *allowed[] = {"schemaVersion", "namespaceId", "gatewayId",
                           "taskId",        "type",        "parameters",
                           "createdAt",     "expiresAt",   "requestDigest"};
  cJSON_ArrayForEach(field, c) {
    bool known = false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); i++)
      if (!strcmp(field->string, allowed[i]))
        known = true;
    if (!known)
      return false;
    for (const cJSON *next = field->next; next; next = next->next)
      if (!strcmp(field->string, next->string))
        return false;
  }
  char *raw = cJSON_PrintUnformatted(c);
  bool fit = raw && strlen(raw) <= 8192;
  free(raw);
  return fit;
}
static void command(const cJSON *c) {
  if (!command_valid(c) || !gw_core_time_trusted())
    return;
  const char *id = str(c, "taskId");
  int64_t expires = parse_utc(str(c, "expiresAt"));
  if (expires < 0)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *r = find_record(id);
  if (r) { /* Duplicate or conflicting command NEVER repeats the physical
              action. */
    if (strcmp(str(r, "requestDigest"), str(c, "requestDigest"))) {
      put(r, "conflictObserved", cJSON_CreateTrue());
      (void)save();
    }
    xSemaphoreGive(mutex);
    return;
  }
  if (cJSON_GetArraySize(records()) >= MAX_TASKS) {
    xSemaphoreGive(mutex);
    return;
  }
  r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "taskId", id);
  cJSON_AddStringToObject(r, "requestDigest", str(c, "requestDigest"));
  cJSON_AddStringToObject(r, "state", "ACCEPTED");
  cJSON_AddStringToObject(r, "phase", "RECEIVED");
  cJSON_AddItemToObject(r, "command", cJSON_Duplicate(c, true));
  cJSON_AddArrayToObject(r, "pending");
  cJSON_AddObjectToObject(r, "phases");
  cJSON_AddNumberToObject(r, "nextSequence", 0);
  /* Reserve worst-case result space before accepting a physical action. */
  char *reserve = malloc(24577);
  if (!reserve) {
    cJSON_Delete(r);
    xSemaphoreGive(mutex);
    return;
  }
  memset(reserve, 'R', 24576);
  reserve[24576] = 0;
  cJSON_AddStringToObject(r, "reserve", reserve);
  free(reserve);
  cJSON_AddItemToArray(records(), r);
  (void)save();
  xSemaphoreGive(mutex);
}
static void execute_ready(const cJSON *c) {
  const char *id = str(c, "taskId");
  int64_t expires = parse_utc(str(c, "expiresAt"));
  cJSON *r;
  esp_err_t e;
  uint64_t now = gw_core_mono_ms(),
           deadline = now + (expires > (int64_t)time(NULL)
                                 ? (uint64_t)(expires - time(NULL)) * 1000
                                 : 0);
  if (deadline <= now) {
    result_for(id, "terminal", "EXPIRED", "NONE", NULL, "TASK_EXPIRED", NULL,
               now + 5000);
    return;
  }
  if (result_for(id, "accepted", "ACCEPTED", "NONE", NULL, NULL, NULL,
                 deadline) != ESP_OK)
    return;
  if (result_for(id, "executing", "EXECUTING", "NONE", NULL, NULL, NULL,
                 deadline) != ESP_OK)
    return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  r = find_record(id);
  text(r, "phase", "INTENT");
  text(r, "executionBoot", boot_id);
  e = save();
  xSemaphoreGive(mutex);
  if (e != ESP_OK)
    return;
  cJSON *details = NULL;
  bool async = false;
  if (!strcmp(str(c, "type"), "CONFIGURE"))
    e = execute_config(c, deadline, &details, &async);
  else if (!strcmp(str(c, "type"), "REBOOT")) {
    if (gw_core_maintenance_acquire(id) != ESP_OK)
      e = ESP_ERR_INVALID_STATE;
    else {
      if (gw_core_mono_ms() >= deadline)
        e = ESP_ERR_TIMEOUT;
      else {
        xSemaphoreTake(mutex, portMAX_DELAY);
        r = find_record(id);
        text(r, "phase", "REBOOT_PENDING");
        text(r, "previousBootId", boot_id);
        e = save();
        xSemaphoreGive(mutex);
        if (e == ESP_OK) {
          if (gw_core_mono_ms() < deadline)
            esp_restart();
          else
            e = ESP_ERR_TIMEOUT;
        }
      }
      gw_core_maintenance_release(id);
    }
  } else
    e = gw_actions_execute(c, deadline, &details);
  if (e == ESP_ERR_NOT_FINISHED)
    async = true; /* trial/recovery obligation remains owned */
  if (!async) {
    if (e == ESP_OK && !strcmp(str(c, "type"), "CONFIGURE"))
      finish_config(c, details, deadline);
    else
      result_for(id, "terminal",
                 e == ESP_OK            ? "SUCCEEDED"
                 : e == ESP_ERR_TIMEOUT ? "UNKNOWN"
                                        : "FAILED",
                 e == ESP_OK ? "VERIFIED" : "NONE", details,
                 e == ESP_OK ? NULL : esp_err_to_name(e), NULL,
                 gw_core_mono_ms() + 5000);
  }
  cJSON_Delete(details);
}
static void service_journal(void) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *snapshot = cJSON_Duplicate(records(), true);
  xSemaphoreGive(mutex);
  cJSON *r;
  cJSON_ArrayForEach(r, snapshot) {
    const char *id = str(r, "taskId");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(r, "command");
    /* Results are replayed oldest first, regardless of task state. */
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(r, "pending");
    const cJSON *v = cJSON_GetArrayItem(pending, 0);
    if (v) {
      const cJSON *proof = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(v, "details"), "networkProbe");
      if (cJSON_IsObject(proof)) {
        /* Only the candidate worker may send a probe, while the tested
         * profile and deadline remain active. Never replay it over a
         * rolled-back interface merely because its name is unchanged. */
        cJSON *tx = gw_core_transaction_by_task(id);
        if (!tx || strcmp(str(tx, "state"), "PROBING")) {
          xSemaphoreTake(mutex, portMAX_DELAY);
          cJSON *live = find_record(id);
          if (live) {
            cJSON *queue = cJSON_GetObjectItemCaseSensitive(live, "pending"),
                  *first = cJSON_GetArrayItem(queue, 0);
            if (first && !strcmp(str(first, "resultId"), str(v, "resultId"))) {
              cJSON *quarantine =
                  cJSON_GetObjectItemCaseSensitive(live, "quarantined");
              if (!quarantine)
                quarantine = cJSON_AddArrayToObject(live, "quarantined");
              cJSON_AddItemToArray(quarantine,
                                   cJSON_DetachItemFromArray(queue, 0));
              save();
            }
          }
          xSemaphoreGive(mutex);
        }
        cJSON_Delete(tx);
        continue;
      }
      deliver(v, NULL, gw_core_mono_ms() + 5000);
      continue;
    }
    if (cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(r, "phases"), "terminal")) {
      if (!strcmp(str(r, "successAck"), "YES"))
        (void)gw_core_platform_ack(id);
      continue;
    }
    if (!strcmp(str(r, "phase"), "RECEIVED")) {
      execute_ready(c);
      continue;
    }
    if (!strcmp(str(r, "phase"), "REBOOT_PENDING") &&
        strcmp(str(r, "previousBootId"), boot_id)) {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "previousBootId", str(r, "previousBootId"));
      cJSON_AddStringToObject(d, "bootId", boot_id);
      result_for(id, "terminal", "SUCCEEDED", "VERIFIED", d, NULL, NULL,
                 gw_core_mono_ms() + 5000);
      cJSON_Delete(d);
      continue;
    }
    cJSON *txn = gw_core_transaction_by_task(id);
    if (txn) {
      if (!strcmp(str(txn, "state"), "COMMITTED")) {
        const cJSON *candidate =
            cJSON_GetObjectItemCaseSensitive(r, "candidate");
        cJSON *recovered = NULL;
        if (!candidate) {
          recovered = configuration();
          merge(recovered,
                cJSON_GetObjectItemCaseSensitive(
                    cJSON_GetObjectItemCaseSensitive(c, "parameters"),
                    "configuration"));
          candidate = recovered;
        }
        if (candidate &&
            gw_store_put_json("cloud/configuration", candidate) == ESP_OK)
          finish_config(c, NULL, gw_core_mono_ms() + 5000);
        cJSON_Delete(recovered);
      } else if (!strcmp(str(txn, "state"), "ROLLED_BACK") ||
                 !strcmp(str(txn, "state"), "FAILED") ||
                 !strcmp(str(txn, "state"), "CANCELLED"))
        result_for(id, "terminal", "FAILED", "NONE", txn, str(txn, "code"),
                   NULL, gw_core_mono_ms() + 5000);
      cJSON_Delete(txn);
      continue;
    }
    if (!strcmp(str(c, "type"), "CONFIGURE")) {
      const cJSON *params = cJSON_GetObjectItemCaseSensitive(c, "parameters");
      const cJSON *patch =
          cJSON_GetObjectItemCaseSensitive(params, "configuration");
      cJSON *cfg = configuration(), *details = NULL;
      merge_configuration(cfg, patch);
      esp_err_t recovered = gw_actions_recover_config(
          patch, cfg, (uint64_t)num(params, "desiredRevision"), &details);
      if (recovered == ESP_OK)
        finish_config(c, details, gw_core_mono_ms() + 5000);
      else if (recovered != ESP_ERR_NOT_FINISHED)
        result_for(id, "terminal",
                   recovered == ESP_ERR_TIMEOUT ? "UNKNOWN" : "FAILED", "NONE",
                   details, esp_err_to_name(recovered), NULL,
                   gw_core_mono_ms() + 5000);
      cJSON_Delete(cfg);
      cJSON_Delete(details);
      continue;
    }
    if (!strcmp(str(c, "type"), "FIRMWARE_UPGRADE") ||
        !strncmp(str(c, "type"), "GATT_", 5)) {
      cJSON *details = NULL;
      esp_err_t recovered =
          gw_actions_recover(c, gw_core_mono_ms() + 30000, &details);
      if (recovered != ESP_ERR_NOT_FINISHED)
        result_for(id, "terminal",
                   recovered == ESP_OK            ? "SUCCEEDED"
                   : recovered == ESP_ERR_TIMEOUT ? "UNKNOWN"
                                                  : "FAILED",
                   recovered == ESP_OK ? "VERIFIED" : "NONE", details,
                   recovered == ESP_OK ? NULL : esp_err_to_name(recovered),
                   NULL, gw_core_mono_ms() + 5000);
      cJSON_Delete(details);
      continue;
    }
    if (!strcmp(str(r, "phase"), "INTENT") ||
        strcmp(str(r, "executionBoot"), boot_id))
      result_for(id, "terminal", "UNKNOWN", "NONE", NULL,
                 "EXECUTION_RECONCILIATION_REQUIRED", NULL,
                 gw_core_mono_ms() + 5000);
  }
  cJSON_Delete(snapshot);
}
static void credentials(void) {
  cJSON *reply = NULL;
  if (gw_http_json("/api/v1/gateway-network-credentials/pending", "GET", NULL,
                   NULL, gw_core_mono_ms() + 5000, &reply) != ESP_OK) {
    cJSON_Delete(reply);
    return;
  }
  cJSON *v;
  cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(reply, "items")) {
    esp_err_t e = gw_core_credential_install(
        str(v, "credentialRef"), str(v, "version"), str(v, "ssid"),
        str(v, "security"), str(v, "password"), str(v, "deliveryId"));
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(v, "password");
    if (cJSON_IsString(secret))
      memset(secret->valuestring, 0, strlen(secret->valuestring));
    if (e != ESP_OK)
      continue;
    cJSON *receipt = cJSON_CreateObject(), *ack = NULL;
    cJSON_AddStringToObject(receipt, "deliveryId", str(v, "deliveryId"));
    cJSON_AddStringToObject(receipt, "credentialRef", str(v, "credentialRef"));
    cJSON_AddStringToObject(receipt, "version", str(v, "version"));
    cJSON_AddStringToObject(receipt, "state", "INSTALLED");
    gw_http_json("/api/v1/gateway-network-credentials/receipts", "POST",
                 receipt, NULL, gw_core_mono_ms() + 5000, &ack);
    cJSON_Delete(receipt);
    cJSON_Delete(ack);
  }
  cJSON_Delete(reply);
}
static void network_state(void) {
  cJSON *state = gw_core_state();
  if (!state)
    return;
  if (!cJSON_IsObject(
          cJSON_GetObjectItemCaseSensitive(state, "desiredLocalNetwork")) ||
      !cJSON_IsObject(
          cJSON_GetObjectItemCaseSensitive(state, "actualNetwork"))) {
    cJSON_Delete(state);
    return;
  }
  const char *applied = str(state, "appliedTaskId");
  if (applied[0]) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    cJSON *r = find_record(applied);
    bool verified = r && !strcmp(str(r, "successAck"), "YES");
    xSemaphoreGive(mutex);
    if (!verified) {
      cJSON_Delete(state);
      return;
    }
  }
  /* Core state also serves local UI: explicitly project the external schema. */
  const char *keys[] = {"networkGeneration",   "localRevision",
                        "desiredLocalNetwork", "actualNetwork",
                        "activeInterface",     "localOverride",
                        "verificationState",   "lastKnownPlatformRevision",
                        "appliedTaskId"};
  cJSON *body = cJSON_CreateObject();
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, keys[i]);
    if (v)
      cJSON_AddItemToObject(body, keys[i], cJSON_Duplicate(v, true));
  }
  cJSON_AddItemToObject(body, "credentials", gw_core_credential_metadata());
  cJSON *reply = NULL;
  gw_http_json("/api/v1/gateway-network-state", "PUT", body, NULL,
               gw_core_mono_ms() + 5000, &reply);
  cJSON_Delete(reply);
  cJSON_Delete(body);
  cJSON_Delete(state);
}
static void heartbeat(void) {
  char observed[32];
  if (!utc(observed))
    return;
  cJSON *body = cJSON_CreateObject(), *reply = NULL;
  cJSON_AddStringToObject(body, "schemaVersion", "1.0");
  cJSON_AddStringToObject(body, "namespaceId", namespace_id);
  cJSON_AddStringToObject(body, "gatewayId", gateway_id);
  cJSON_AddStringToObject(body, "observedAt", observed);
  cJSON_AddStringToObject(body, "firmwareVersion", esp_app_get_description()->version);
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON_AddNumberToObject(body, "reportedConfigurationRevision",
                          num(journal, "reportedRevision"));
  xSemaphoreGive(mutex);
  cJSON_AddNumberToObject(body, "spoolBytes", gw_capture_used_bytes());
  cJSON_AddStringToObject(body, "timeQuality", "SYNCED");
  cJSON_AddStringToObject(
      body, "health", gw_nrf_identity(NULL, NULL, NULL) ? "OK" : "DEGRADED");
  gw_http_json("/api/v1/gateway-ingress/status", "POST", body, NULL,
               gw_core_mono_ms() + 5000, &reply);
  cJSON_Delete(reply);
  cJSON_Delete(body);
}
static void cloud_task(void *unused) {
  (void)unused;
  unsigned tick = 0;
  for (;;) {
    if (identity_ready && gw_core_time_trusted()) {
      service_journal();
      if (tick % 5 == 0) {
        credentials();
        network_state();
        heartbeat();
      }
      cJSON *reply = NULL;
      if (gw_http_json("/api/v1/gateway-ingress/commands", "GET", NULL, NULL,
                       gw_core_mono_ms() + 5000, &reply) == ESP_OK) {
        const cJSON *c;
        cJSON_ArrayForEach(
            c, cJSON_GetObjectItemCaseSensitive(reply, "commands")) command(c);
      }
      cJSON_Delete(reply);
    }
    ++tick;
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
static void upload_task(void *unused) {
  (void)unused;
  for (;;) {
    if (identity_ready && gw_core_time_trusted())
      gw_capture_upload(namespace_id, gateway_id, gw_core_mono_ms() + 5000);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
esp_err_t gw_cloud_init(void) {
  mutex = xSemaphoreCreateMutex();
  if (!mutex)
    return ESP_ERR_NO_MEM;
  journal = gw_store_get_json(JOURNAL_KEY);
  if (!journal) {
    journal = cJSON_CreateObject();
    cJSON_AddArrayToObject(journal, "records");
    cJSON_AddNumberToObject(journal, "reportedRevision", 0);
  }
  random_id(boot_id, 16);
  text(journal, "bootId", boot_id);
  esp_err_t e = save();
  if (e != ESP_OK)
    return e;
  identity_ready = gw_factory_get_string("namespace", namespace_id,
                                         sizeof(namespace_id)) == ESP_OK &&
                   gw_factory_get_string("gateway", gateway_id,
                                         sizeof(gateway_id)) == ESP_OK &&
                   valid_id(namespace_id) && valid_id(gateway_id);
  gw_core_set_probe_handler(probe, NULL);
  if (xTaskCreate(upload_task, "gw_upload", 16384, NULL, 4, NULL) != pdPASS)
    return ESP_ERR_NO_MEM;
  return xTaskCreate(cloud_task, "gw_cloud", 16384, NULL, 5, NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
