#include "gw_peripheral.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_authority.h"
#include "gw_ble_peripheral.h"
#include "gw_capture.h"
#include "gw_core.h"
#include "gw_nrf.h"
#include "gw_pg.h"
#include "gw_transport.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "protocomm_security2.h"
#include "session.pb-c.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fixed resource limits fail closed. Ledger entries are never silently evicted:
 * deleting business dedup evidence would authorize repeated side effects. */
#define P_CLIENTS 8
#define P_EVENTS 8
#define P_LEDGER 64
#define P_TASKS 16
#define P_SESSION_MS 600000U
#define P_STORE "peripheral/state"
typedef struct {
  bool used, authenticated;
  uint8_t radio, boot[16], salt[16], verifier[384];
  uint32_t connection, tx_sequence;
  uint16_t mtu;
  uint64_t expires, low_sequence, tx_deadline, received_utc_ms, captured_ms;
  char reference[33], client[65];
  cJSON *routes;
  protocomm_security_handle_t security;
  protocomm_security2_params_t params;
  gw_pg_rx_t rx;
  uint8_t *tx;
  size_t tx_length, tx_offset;
  uint8_t tx_type;
  bool tx_pending;
} session_t;
typedef struct {
  uint32_t connection;
  uint64_t captured_ms;
  uint16_t mtu, length;
  uint8_t bytes[512];
} local_input_t;
static SemaphoreHandle_t mutex, upload_mutex;
static QueueHandle_t local_inputs;
static session_t sessions[2];
static cJSON *journal, *acl;
static char namespace_id[129], gateway_id[129];
static bool ready, disabled;
static uint64_t next_upload, low_next, low_limit;

static const char *str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static cJSON *item(cJSON *o, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(o, key);
}
static bool number(const cJSON *o, const char *key, uint64_t max,
                   uint64_t *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
      v->valuedouble > max || floor(v->valuedouble) != v->valuedouble)
    return false;
  *out = (uint64_t)v->valuedouble;
  return true;
}
static bool setstr(cJSON *o, const char *key, const char *value) {
  cJSON *replacement = cJSON_CreateString(value);
  if (!replacement)
    return false;
  bool okay = item(o, key)
                  ? cJSON_ReplaceItemInObjectCaseSensitive(o, key, replacement)
                  : cJSON_AddItemToObject(o, key, replacement);
  if (!okay)
    cJSON_Delete(replacement);
  return okay;
}
static bool keys(const cJSON *o, const char *const *allowed, size_t count) {
  if (!cJSON_IsObject(o))
    return false;
  for (const cJSON *p = o->child; p; p = p->next) {
    bool known = false;
    for (size_t i = 0; i < count; i++)
      known |= !strcmp(p->string, allowed[i]);
    if (!known)
      return false;
    for (const cJSON *q = p->next; q; q = q->next)
      if (!strcmp(p->string, q->string))
        return false;
  }
  return true;
}
static bool unhex(const char *p, uint8_t *out, size_t n) {
  if (strlen(p) != 2 * n)
    return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v = 0;
    for (unsigned j = 0; j < 2; j++) {
      char c = p[2 * i + j];
      if (c >= '0' && c <= '9')
        v = 16 * v + c - '0';
      else if (c >= 'a' && c <= 'f')
        v = 16 * v + c - 'a' + 10;
      else
        return false;
    }
    out[i] = (uint8_t)v;
  }
  return true;
}
static void hex(const uint8_t *in, size_t n, char *out) {
  static const char h[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = h[in[i] >> 4];
    out[2 * i + 1] = h[in[i] & 15];
  }
  out[2 * n] = 0;
}
static bool business_id(const char *s) {
  uint8_t bytes[16];
  return unhex(s, bytes, 16);
}
static void random_id(char out[33]) {
  uint8_t b[16];
  esp_fill_random(b, 16);
  hex(b, 16, out);
}
static bool payload_valid(const char *s) {
  size_t n = strlen(s), written = 0;
  if (n > 2732 || n % 4)
    return false;
  uint8_t raw[2048];
  char canonical[2733];
  if (mbedtls_base64_decode(raw, sizeof(raw), &written,
                            (const unsigned char *)s, n))
    return false;
  size_t len = 0;
  if (mbedtls_base64_encode((unsigned char *)canonical, sizeof(canonical), &len,
                            raw, written))
    return false;
  canonical[len] = 0;
  return !strcmp(canonical, s);
}
static cJSON *parse_json(const uint8_t *p, size_t n) {
  if (!p || !n || n > GW_PG_MAX || memchr(p, 0, n))
    return NULL;
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (quoted) {
      if (escaped) {
        if (c == 'u' && i + 4 < n && !memcmp(p + i + 1, "0000", 4))
          return NULL;
        escaped = false;
      } else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 4)
        return NULL;
    } else if (c == '}' || c == ']') {
      if (!depth)
        return NULL;
      --depth;
    }
  }
  if (quoted || depth)
    return NULL;
  char *copy = malloc(n + 1);
  if (!copy)
    return NULL;
  memcpy(copy, p, n);
  copy[n] = 0;
  cJSON *o = cJSON_ParseWithLengthOpts(copy, n + 1, NULL, true);
  mbedtls_platform_zeroize(copy, n);
  free(copy);
  return o;
}
static bool route_allowed(const session_t *s, const char *route) {
  if (!route[0] || strlen(route) > 128)
    return false;
  const cJSON *p;
  cJSON_ArrayForEach(p,
                     s->routes) if (cJSON_IsString(p) &&
                                    !strcmp(p->valuestring, route)) return true;
  return false;
}
static void connection_ref(uint8_t radio, const uint8_t boot[16],
                           uint32_t connection, char out[33]) {
  uint8_t bytes[39], hash[32];
  memcpy(bytes, "gw32-peripheral-v1", 18);
  bytes[18] = radio;
  memcpy(bytes + 19, boot, 16);
  gw_put_u32(bytes + 35, connection);
  mbedtls_sha256(bytes, sizeof(bytes), hash, 0);
  hex(hash, 16, out);
}
static void count(const char *key) {
  cJSON *v = item(journal, key);
  if (cJSON_IsNumber(v))
    cJSON_SetNumberValue(v, v->valuedouble + 1);
  else if (!v)
    (void)cJSON_AddNumberToObject(journal, key, 1);
}
static esp_err_t persist(void) { return gw_store_put_json(P_STORE, journal); }
static void rollback(void) {
  cJSON *saved = gw_store_get_json(P_STORE);
  if (saved) {
    cJSON_Delete(journal);
    journal = saved;
  } else
    ready = false;
}
/* A failed commit discards the speculative in-memory journal. Never emit STORED
 * from a copy that was not durably published. */
static esp_err_t commit(void) {
  esp_err_t e = persist();
  if (e != ESP_OK)
    rollback();
  return e;
}
static void secure_free(void *p, size_t n) {
  if (p) {
    mbedtls_platform_zeroize(p, n);
    free(p);
  }
}
static void session_drop(session_t *s) {
  if (s->security) {
    protocomm_security2.close_transport_session(s->security, s->connection);
    protocomm_security2.cleanup(s->security);
  }
  cJSON_Delete(s->routes);
  secure_free(s->tx, s->tx_length);
  mbedtls_platform_zeroize(s, sizeof(*s));
}
static bool live(session_t *s) {
  uint8_t boot[16], address[6], type, security;
  uint16_t mtu;
  if (s->radio) {
    if (!gw_ble_peripheral_identity(s->connection, boot, address, &type,
                                    &security, &mtu) ||
        memcmp(boot, s->boot, 16))
      return false;
  } else {
    if (!gw_nrf_identity(boot, NULL, NULL) || memcmp(boot, s->boot, 16))
      return false;
    uint8_t status, state[16];
    size_t length = sizeof(state);
    if (gw_nrf_command(0x0d, NULL, 0, &status, state, &length,
                       gw_core_mono_ms() + 250) != ESP_OK ||
        status || length != 16 || gw_get_u32(state) != s->connection ||
        state[4] != 1)
      return false;
    mtu = gw_get_u16(state + 12);
  }
  s->mtu = mtu;
  return mtu >= 23;
}
static esp_err_t close_link(session_t *s) {
  if (s->radio)
    return gw_ble_peripheral_close(s->connection);
  uint8_t args[4], status;
  size_t n = 0;
  gw_put_u32(args, s->connection);
  esp_err_t e = gw_nrf_command(0x0f, args, sizeof(args), &status, NULL, &n,
                               gw_core_mono_ms() + 250);
  return e ? e : status ? ESP_ERR_INVALID_STATE : ESP_OK;
}
static session_t *session_get(uint8_t radio, const uint8_t boot[16],
                              uint32_t connection) {
  if (!gw_authority_has("PERIPHERAL_BRIDGE_1") ||
      !gw_authority_board_verified(radio ? "ESP32_BLE_PERIPHERAL"
                                         : "NRF54_BLE_PERIPHERAL"))
    return NULL;
  if (radio > 1 || !connection)
    return NULL;
  session_t *s = &sessions[radio];
  if (s->used && (s->connection != connection || memcmp(s->boot, boot, 16)))
    session_drop(s);
  if (!s->used) {
    s->used = true;
    s->radio = radio;
    s->connection = connection;
    memcpy(s->boot, boot, 16);
    s->expires = gw_core_mono_ms() + P_SESSION_MS;
    gw_pg_init(&s->rx);
    connection_ref(radio, boot, connection, s->reference);
  }
  if (gw_core_mono_ms() >= s->expires || !live(s)) {
    session_drop(s);
    return NULL;
  }
  return s;
}
static esp_err_t queue_tx(session_t *s, uint8_t type, uint8_t *bytes,
                          size_t n) {
  if (!bytes || !n || n > 4096 || s->tx_pending ||
      s->tx_sequence == UINT32_MAX) {
    secure_free(bytes, n);
    return ESP_ERR_INVALID_STATE;
  }
  secure_free(s->tx, s->tx_length);
  s->tx = bytes;
  s->tx_length = n;
  s->tx_offset = 0;
  s->tx_type = type;
  ++s->tx_sequence;
  s->tx_pending = true;
  s->low_sequence = 0;
  s->tx_deadline = gw_core_mono_ms() + 5000;
  return ESP_OK;
}
static esp_err_t queue_json(session_t *s, uint8_t type, cJSON *object) {
  char *plain = cJSON_PrintUnformatted(object);
  if (!plain)
    return ESP_ERR_NO_MEM;
  uint8_t *encrypted = NULL;
  ssize_t n = 0;
  esp_err_t e = protocomm_security2.encrypt(s->security, s->connection,
                                            (const uint8_t *)plain,
                                            strlen(plain), &encrypted, &n);
  secure_free(plain, strlen(plain));
  if (e != ESP_OK) {
    secure_free(encrypted, n > 0 ? (size_t)n : 0);
    return e;
  }
  return queue_tx(s, type, encrypted, n > 0 ? (size_t)n : 0);
}
static esp_err_t ack_phone(session_t *s, const char *business,
                           const char *status) {
  cJSON *o = cJSON_CreateObject();
  if (!o)
    return ESP_ERR_NO_MEM;
  if (!cJSON_AddStringToObject(o, "schemaVersion", "1.0") ||
      !cJSON_AddNumberToObject(o, "transportSeq", (double)s->tx_sequence + 1) ||
      !cJSON_AddStringToObject(o, "inReplyTo", business) ||
      !cJSON_AddStringToObject(o, "status", status)) {
    cJSON_Delete(o);
    return ESP_ERR_NO_MEM;
  }
  esp_err_t e = queue_json(s, 2, o);
  cJSON_Delete(o);
  return e;
}
static bool authorize(session_t *s, const uint8_t *username, size_t length) {
  if (!length || length > 64)
    return false;
  const cJSON *clients = cJSON_GetObjectItemCaseSensitive(acl, "clients"),
              *match = NULL, *c;
  cJSON_ArrayForEach(
      c,
      clients) if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,
                                                                 "enabled")) &&
                   strlen(str(c, "username")) == length &&
                   !memcmp(str(c, "username"), username, length)) {
    if (match)
      return false;
    match = c;
  }
  uint64_t slot;
  if (!match || !number(match, "slot", P_CLIENTS - 1, &slot) ||
      !*str(match, "clientId") || strlen(str(match, "clientId")) > 64)
    return false;
  const cJSON *routes = cJSON_GetObjectItemCaseSensitive(match, "routes");
  if (!cJSON_IsArray(routes) || !cJSON_GetArraySize(routes))
    return false;
  char key[16];
  size_t salt = 16, verifier = 384;
  snprintf(key, sizeof(key), "periph%u_salt", (unsigned)slot);
  if (gw_factory_get_blob(key, s->salt, &salt) != ESP_OK || salt != 16)
    return false;
  snprintf(key, sizeof(key), "periph%u_ver", (unsigned)slot);
  if (gw_factory_get_blob(key, s->verifier, &verifier) != ESP_OK ||
      verifier != 384)
    return false;
  snprintf(s->client, sizeof(s->client), "%s", str(match, "clientId"));
  s->routes = cJSON_Duplicate(routes, true);
  s->params = (protocomm_security2_params_t){.salt = (char *)s->salt,
                                             .salt_len = 16,
                                             .verifier = (char *)s->verifier,
                                             .verifier_len = 384};
  return s->routes && protocomm_security2.init(&s->security) == ESP_OK &&
         protocomm_security2.new_transport_session(s->security,
                                                   s->connection) == ESP_OK;
}
static esp_err_t handshake(session_t *s) {
  SessionData *request = session_data__unpack(NULL, s->rx.total, s->rx.bytes);
  unsigned stage = 99;
  if (request && request->sec_ver == SEC_SCHEME_VERSION__SecScheme2 &&
      request->proto_case == SESSION_DATA__PROTO_SEC2 && request->sec2) {
    Sec2Payload *p = request->sec2;
    if (p->msg == SEC2_MSG_TYPE__S2Session_Command0 &&
        p->payload_case == SEC2_PAYLOAD__PAYLOAD_SC0 && p->sc0 &&
        p->sc0->client_pubkey.len == 384 && !s->security &&
        authorize(s, p->sc0->client_username.data, p->sc0->client_username.len))
      stage = 0;
    else if (p->msg == SEC2_MSG_TYPE__S2Session_Command1 &&
             p->payload_case == SEC2_PAYLOAD__PAYLOAD_SC1 && p->sc1 &&
             p->sc1->client_proof.len == 64 && s->security && !s->authenticated)
      stage = 1;
  }
  if (request)
    session_data__free_unpacked(request, NULL);
  if (stage == 99)
    return ESP_ERR_INVALID_STATE;
  uint8_t *out = NULL;
  ssize_t n = 0;
  esp_err_t e = protocomm_security2.security_req_handler(
      s->security, &s->params, s->connection, s->rx.bytes, s->rx.total, &out,
      &n, NULL);
  if (e != ESP_OK) {
    secure_free(out, n > 0 ? (size_t)n : 0);
    return e;
  }
  if (stage == 1)
    s->authenticated = true;
  return queue_tx(s, 0, out, n > 0 ? (size_t)n : 0);
}
static bool digest_business(const session_t *s, const cJSON *o, char out[65]) {
  /* Fixed-order JSON is unambiguous; transport metadata is intentionally
   * absent. */
  cJSON *d = cJSON_CreateArray();
  if (!d)
    return false;
  const char *values[] = {s->client,
                          "UP",
                          str(o, "businessId"),
                          str(o, "route"),
                          str(o, "payloadBase64"),
                          str(o, "inReplyTo")};
  for (size_t i = 0; i < 6; i++) {
    cJSON *value = cJSON_CreateString(values[i]);
    if (!value || !cJSON_AddItemToArray(d, value)) {
      cJSON_Delete(value);
      cJSON_Delete(d);
      return false;
    }
  }
  char *p = cJSON_PrintUnformatted(d);
  uint8_t hash[32];
  bool valid = p != NULL && cJSON_GetArraySize(d) == 6;
  if (valid)
    valid = mbedtls_sha256((uint8_t *)p, strlen(p), hash, 0) == 0;
  if (valid)
    hex(hash, 32, out);
  free(p);
  cJSON_Delete(d);
  return valid;
}
static cJSON *find_task(const char *id) {
  cJSON *p;
  cJSON_ArrayForEach(
      p, item(journal, "tasks")) if (!strcmp(str(p, "taskId"), id)) return p;
  return NULL;
}
static esp_err_t receive_ack(session_t *s, const cJSON *o) {
  static const char *const fields[] = {"schemaVersion", "transportSeq",
                                       "inReplyTo", "status", "responseBase64"};
  uint64_t sequence;
  const char *status = str(o, "status");
  if (!keys(o, fields, 5) || strcmp(str(o, "schemaVersion"), "1.0") ||
      !number(o, "transportSeq", UINT32_MAX, &sequence) ||
      sequence != s->rx.sequence || !business_id(str(o, "inReplyTo")) ||
      (strcmp(status, "STORED") && strcmp(status, "APPLIED") &&
       strcmp(status, "REJECTED")) ||
      (cJSON_GetObjectItemCaseSensitive(o, "responseBase64") &&
       (!cJSON_IsString(
            cJSON_GetObjectItemCaseSensitive(o, "responseBase64")) ||
        !payload_valid(str(o, "responseBase64")))))
    return ESP_ERR_INVALID_ARG;
  cJSON *t;
  cJSON_ArrayForEach(
      t,
      item(journal,
           "tasks")) if (!strcmp(str(t, "businessId"), str(o, "inReplyTo")) &&
                         !strcmp(str(t, "clientId"), s->client) &&
                         !strcmp(str(t, "connectionRef"), s->reference) &&
                         !strcmp(str(t, "state"), "EXECUTING")) {
    uint64_t deadline = 0;
    if (!number(t, "deadlineMs", UINT64_C(9007199254740991), &deadline) ||
        s->captured_ms >= deadline) {
      setstr(t, "state", "UNKNOWN");
      return ESP_ERR_TIMEOUT;
    }
    cJSON *next = cJSON_Duplicate(t, true);
    if (!next)
      return ESP_ERR_NO_MEM;
    bool okay = (!strcmp(status, "STORED") || setstr(next, "state", status)) &&
                setstr(next, "businessStatus", status);
    if (cJSON_GetObjectItemCaseSensitive(o, "responseBase64"))
      okay = okay && setstr(next, "responseBase64", str(o, "responseBase64"));
    if (!okay ||
        !cJSON_ReplaceItemViaPointer(item(journal, "tasks"), t, next)) {
      cJSON_Delete(next);
      return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}
static esp_err_t receive_business(session_t *s, const cJSON *o,
                                  char accepted[33]) {
  static const char *const fields[] = {"schemaVersion", "transportSeq",
                                       "businessId",    "inReplyTo",
                                       "route",         "payloadBase64"};
  uint64_t sequence;
  if (!keys(o, fields, 6) || strcmp(str(o, "schemaVersion"), "1.0") ||
      !number(o, "transportSeq", UINT32_MAX, &sequence) ||
      sequence != s->rx.sequence || !business_id(str(o, "businessId")) ||
      (item((cJSON *)o, "inReplyTo") && !business_id(str(o, "inReplyTo"))) ||
      !route_allowed(s, str(o, "route")) ||
      !cJSON_IsString(item((cJSON *)o, "payloadBase64")) ||
      !payload_valid(str(o, "payloadBase64")))
    return ESP_ERR_INVALID_ARG;
  char digest[65];
  if (!digest_business(s, o, digest))
    return ESP_ERR_NO_MEM;
  cJSON *l;
  cJSON_ArrayForEach(
      l, item(journal, "ledger")) if (!strcmp(str(l, "clientId"), s->client) &&
                                      !strcmp(str(l, "businessId"),
                                              str(o, "businessId"))) {
    if (strcmp(str(l, "digest"), digest))
      return ESP_ERR_INVALID_STATE;
    snprintf(accepted, 33, "%s", str(o, "businessId"));
    return ESP_OK;
  }
  if (cJSON_GetArraySize(item(journal, "events")) >= P_EVENTS ||
      cJSON_GetArraySize(item(journal, "ledger")) >= P_LEDGER)
    return ESP_ERR_NO_MEM;
  char event_id[33], batch[33], boot[33];
  random_id(event_id);
  random_id(batch);
  hex(s->boot, 16, boot);
  cJSON *event = cJSON_CreateObject(), *spool = cJSON_CreateObject(),
        *ledger = cJSON_CreateObject();
  if (!event || !spool || !ledger) {
    cJSON_Delete(event);
    cJSON_Delete(spool);
    cJSON_Delete(ledger);
    return ESP_ERR_NO_MEM;
  }
  bool okay =
      cJSON_AddStringToObject(event, "eventId", event_id) &&
      cJSON_AddStringToObject(event, "radio", s->radio ? "ESP32" : "NRF54") &&
      cJSON_AddStringToObject(event, "controllerBootId", boot) &&
      cJSON_AddStringToObject(event, "connectionRef", s->reference) &&
      cJSON_AddStringToObject(event, "clientId", s->client) &&
      cJSON_AddNumberToObject(event, "transportSeq", sequence) &&
      cJSON_AddStringToObject(event, "businessId", str(o, "businessId")) &&
      cJSON_AddStringToObject(event, "route", str(o, "route")) &&
      cJSON_AddStringToObject(event, "payloadBase64", str(o, "payloadBase64"));
  char timestamp[32] = {0};
  if (s->received_utc_ms) {
    time_t seconds = (time_t)(s->received_utc_ms / 1000);
    struct tm utc;
    char base[24];
    if (gmtime_r(&seconds, &utc) &&
        strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc))
      snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", base,
               (unsigned)(s->received_utc_ms % 1000));
  }
  okay = okay && cJSON_AddStringToObject(event, "timeQuality",
                                         timestamp[0] ? "SYNCED" : "UNSYNCED");
  if (timestamp[0])
    okay = okay && cJSON_AddStringToObject(event, "receivedAt", timestamp);
  if (*str(o, "inReplyTo"))
    okay = okay &&
           cJSON_AddStringToObject(event, "inReplyTo", str(o, "inReplyTo"));
  okay = okay && cJSON_AddStringToObject(spool, "batchId", batch) &&
         cJSON_AddStringToObject(spool, "state", "PENDING") &&
         cJSON_AddStringToObject(ledger, "clientId", s->client) &&
         cJSON_AddStringToObject(ledger, "businessId", str(o, "businessId")) &&
         cJSON_AddStringToObject(ledger, "digest", digest);
  if (!okay || !cJSON_AddItemToObject(spool, "event", event)) {
    cJSON_Delete(event);
    cJSON_Delete(spool);
    cJSON_Delete(ledger);
    return ESP_ERR_NO_MEM;
  }
  /* All allocations succeeded. Array linking itself allocates no memory. */
  if (!cJSON_AddItemToArray(item(journal, "events"), spool)) {
    cJSON_Delete(spool);
    cJSON_Delete(ledger);
    return ESP_ERR_NO_MEM;
  }
  if (!cJSON_AddItemToArray(item(journal, "ledger"), ledger)) {
    cJSON_DetachItemViaPointer(item(journal, "events"), spool);
    cJSON_Delete(spool);
    cJSON_Delete(ledger);
    return ESP_ERR_NO_MEM;
  }
  snprintf(accepted, 33, "%s", str(o, "businessId"));
  return ESP_OK;
}

static bool mark_volatile(const session_t *s) {
  cJSON *refs = item(journal, "volatileSessions");
  for (int i = cJSON_GetArraySize(refs); i < 2; i++) {
    cJSON *empty = cJSON_CreateNull();
    if (!empty || !cJSON_AddItemToArray(refs, empty)) {
      cJSON_Delete(empty);
      return false;
    }
  }
  cJSON *ref = cJSON_CreateString(s->reference);
  if (!ref || !cJSON_ReplaceItemInArray(refs, s->radio, ref)) {
    cJSON_Delete(ref);
    return false;
  }
  return true;
}
static void consume(session_t *s, const uint8_t *bytes, size_t n,
                    const char *receipt, uint64_t captured_ms) {
  char accepted[33] = {0};
  esp_err_t e = ESP_OK;
  int parsed = GW_PG_INVALID;
  if (s)
    s->captured_ms = captured_ms;
  if (s && captured_ms <= gw_core_mono_ms() &&
      gw_core_mono_ms() - captured_ms < 5000)
    parsed = gw_pg_receive(&s->rx, bytes, n, s->mtu, captured_ms);
  if (parsed == GW_PG_COMPLETE) {
    if (s->tx_pending)
      e = ESP_ERR_INVALID_STATE; /* one exchange in flight per direction */
    else if (s->rx.type == 0)
      e = handshake(s);
    else if (!s->authenticated)
      e = ESP_ERR_INVALID_STATE;
    else {
      uint8_t *plain = NULL;
      ssize_t length = 0;
      e = protocomm_security2.decrypt(s->security, s->connection, s->rx.bytes,
                                      s->rx.total, &plain, &length);
      cJSON *o =
          e == ESP_OK && length > 0 ? parse_json(plain, (size_t)length) : NULL;
      secure_free(plain, length > 0 ? (size_t)length : 0);
      if (e == ESP_OK)
        e = o ? (s->rx.type == 1 ? receive_business(s, o, accepted)
                                 : receive_ack(s, o))
              : ESP_ERR_INVALID_ARG;
      cJSON_Delete(o);
    }
  } else if (parsed == GW_PG_DUPLICATE) {
    /* Retransmit only the exact cached ciphertext with its original outbound
     * sequence. Never decrypt a retry a second time or reuse a new nonce. */
    if (s->tx && !s->rx.active && !s->tx_pending) {
      s->tx_offset = 0;
      s->tx_pending = true;
      s->low_sequence = 0;
      s->tx_deadline = gw_core_mono_ms() + 5000;
    }
  } else if (parsed != GW_PG_MORE)
    e = ESP_ERR_INVALID_STATE;
  bool metadata_ok =
      !receipt || !*receipt || setstr(journal, "lastReceipt", receipt);
  if (e != ESP_OK) {
    count("rejectedFragments");
    metadata_ok =
        metadata_ok && setstr(journal, "lastDisposition", "SESSION_LOST");
    count("sessionLost");
  } else {
    metadata_ok = metadata_ok && mark_volatile(s) &&
                  setstr(journal, "lastDisposition",
                         accepted[0] ? "STORED" : "SESSION_BOUND");
  }
  esp_err_t saved;
  if (metadata_ok)
    saved = commit();
  else {
    rollback();
    saved = ESP_ERR_NO_MEM;
  }
  if (saved == ESP_OK && receipt && *receipt)
    (void)gw_capture_peripheral_ack(receipt);
  if (saved != ESP_OK || e != ESP_OK) {
    if (s) {
      (void)close_link(s);
      session_drop(s);
    }
    return;
  }
  if (accepted[0] && ack_phone(s, accepted, "STORED") != ESP_OK) {
    (void)close_link(s);
    session_drop(s);
  }
}
static void pump_tx(session_t *s) {
  if (!s->used || !s->tx_pending)
    return;
  if (gw_core_mono_ms() >= s->tx_deadline || !live(s)) {
    (void)close_link(s);
    session_drop(s);
    return;
  }
  uint8_t fragment[512];
  size_t n =
      gw_pg_fragment(fragment, sizeof(fragment), s->tx_type, s->tx_sequence,
                     s->tx, s->tx_length, s->tx_offset, s->mtu);
  if (!n) {
    session_drop(s);
    return;
  }
  if (!s->low_sequence) {
    if (!low_next || low_next > low_limit) {
      const char *stored = str(journal, "lowSequence");
      char *end = NULL;
      uint64_t old = strtoull(stored, &end, 10);
      if (!*stored || *end || old > UINT64_MAX - 64) {
        session_drop(s);
        return;
      }
      char text[24];
      snprintf(text, sizeof(text), "%" PRIu64, old + 64);
      if (!setstr(journal, "lowSequence", text) || commit() != ESP_OK) {
        session_drop(s);
        return;
      }
      /* Reserve a durable block before use. After reset, skip unused numbers;
       * UART accepts increasing u64 values, and per-fragment NVS wear is
       * avoided. */
      low_next = old + 1;
      low_limit = old + 64;
    }
    s->low_sequence = low_next++;
  }
  esp_err_t e;
  if (s->radio)
    e = gw_ble_peripheral_send(s->connection, s->low_sequence, fragment, n);
  else {
    uint8_t args[526], status = 255, out[8];
    size_t length = sizeof(out);
    gw_put_u64(args, s->low_sequence);
    gw_put_u32(args + 8, s->connection);
    gw_put_u16(args + 12, (uint16_t)n);
    memcpy(args + 14, fragment, n);
    e = gw_nrf_command(0x0e, args, n + 14, &status, out, &length,
                       gw_core_mono_ms() + 250);
    if (e == ESP_OK && status == GW_BUSY)
      return;
    if (e == ESP_OK &&
        (status || length != 8 || gw_get_u64(out) != s->low_sequence))
      e = ESP_ERR_INVALID_RESPONSE;
  }
  if (e == ESP_ERR_NO_MEM || e == ESP_ERR_NOT_FINISHED)
    return;
  if (e != ESP_OK) {
    (void)close_link(s);
    session_drop(s);
    return;
  }
  s->tx_offset += n - 12;
  s->low_sequence = 0;
  if (s->tx_offset == s->tx_length)
    s->tx_pending = false;
}
static esp_err_t local_receive(uint32_t connection, const uint8_t *p, size_t n,
                               uint16_t mtu, void *unused) {
  (void)unused;
  if (n > 512 || (!p && n) || !local_inputs)
    return ESP_ERR_INVALID_ARG;
  local_input_t input = {.connection = connection,
                         .captured_ms = gw_core_mono_ms(),
                         .mtu = mtu,
                         .length = (uint16_t)n};
  if (n)
    memcpy(input.bytes, p, n);
  return xQueueSend(local_inputs, &input, 0) == pdTRUE ? ESP_OK
                                                       : ESP_ERR_NO_MEM;
}
static void process_inputs(void) {
  /* Called only while mutex is held. Each receive was published atomically by
   * its producer. Live boot/connection checks precede any session creation. */
  for (unsigned i = 0; i < 32; i++) {
    cJSON *in = gw_capture_peripheral_peek();
    if (!in)
      break;
    const char *token = str(in, "receiptToken");
    if (*token && !strcmp(token, str(journal, "lastReceipt"))) {
      (void)gw_capture_peripheral_ack(token);
      cJSON_Delete(in);
      continue;
    }
    uint8_t boot[16], bytes[512], ingress_boot[16], current_boot[16];
    size_t n = 0;
    uint64_t connection = 0, arrived = 0, utc_ms = 0;
    gw_capture_esp_boot(current_boot);
    bool valid =
        !strcmp(str(in, "radio"), "NRF54") &&
        unhex(str(in, "controllerBootId"), boot, 16) &&
        unhex(str(in, "ingressBootId"), ingress_boot, 16) &&
        !memcmp(ingress_boot, current_boot, 16) &&
        number(in, "arrivedMs", UINT64_C(9007199254740991), &arrived) &&
        number(in, "connectionId", UINT32_MAX, &connection) && connection &&
        !mbedtls_base64_decode(bytes, sizeof(bytes), &n,
                               (const unsigned char *)str(in, "payloadBase64"),
                               strlen(str(in, "payloadBase64")));
    session_t *s = valid ? session_get(0, boot, (uint32_t)connection) : NULL;
    if (s) {
      (void)number(in, "receivedUtcMs", UINT64_C(9007199254740991), &utc_ms);
      s->received_utc_ms = utc_ms;
    }
    consume(s, bytes, valid ? n : 0, token, arrived);
    cJSON_Delete(in);
    if (!ready)
      break;
  }
  local_input_t input;
  for (unsigned i = 0;
       i < 16 && xQueueReceive(local_inputs, &input, 0) == pdTRUE; i++) {
    if (!input.length) {
      if (sessions[1].used && sessions[1].connection == input.connection)
        session_drop(&sessions[1]);
      continue;
    }
    uint8_t boot[16], address[6], type, security;
    uint16_t mtu;
    session_t *s = gw_ble_peripheral_identity(input.connection, boot, address,
                                              &type, &security, &mtu)
                       ? session_get(1, boot, input.connection)
                       : NULL;
    if (s) {
      uint64_t lower, upper;
      s->received_utc_ms = 0;
      if (gw_core_time_bounds(&lower, &upper) &&
          gw_core_mono_ms() >= input.captured_ms) {
        uint64_t utc = lower + (upper - lower) / 2,
                 age = gw_core_mono_ms() - input.captured_ms;
        if (utc > age)
          s->received_utc_ms = utc - age;
      }
    }
    consume(s, input.bytes, input.length, NULL, input.captured_ms);
    if (!ready)
      break;
  }
  for (unsigned i = 0; i < 2; i++) {
    session_t *s = &sessions[i];
    if (!s->used)
      continue;
    if (gw_core_mono_ms() >= s->expires ||
        (s->rx.active && gw_core_mono_ms() >= s->rx.deadline)) {
      count("sessionLost");
      setstr(journal, "lastDisposition", "SESSION_LOST");
      (void)commit();
      (void)close_link(s);
      session_drop(s);
      continue;
    }
    pump_tx(s);
  }
}
static void worker(void *unused) {
  (void)unused;
  for (;;) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (ready)
      process_inputs();
    xSemaphoreGive(mutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
esp_err_t gw_peripheral_init(void) {
  if (ready || disabled)
    return ESP_OK;
  low_next = low_limit = 0;
  if (!gw_authority_has("PERIPHERAL_BRIDGE_1")) {
    disabled = true;
    return ESP_OK;
  }
  if (protocomm_security2.patch_ver != 1)
    return ESP_ERR_NOT_SUPPORTED;
  if (gw_factory_get_string("namespace", namespace_id, sizeof(namespace_id)) !=
          ESP_OK ||
      gw_factory_get_string("gateway", gateway_id, sizeof(gateway_id)) !=
          ESP_OK)
    return ESP_ERR_INVALID_STATE;
  mutex = xSemaphoreCreateMutex();
  upload_mutex = xSemaphoreCreateMutex();
  local_inputs = xQueueCreate(16, sizeof(local_input_t));
  if (!mutex || !upload_mutex || !local_inputs)
    return ESP_ERR_NO_MEM;
  char *clients = malloc(8192);
  if (!clients)
    return ESP_ERR_NO_MEM;
  if (gw_factory_get_string("periph_acl", clients, 8192) == ESP_OK)
    acl = cJSON_Parse(clients);
  secure_free(clients, 8192);
  if (!acl)
    acl = cJSON_CreateObject(); /* no provisioned business clients means deny
                                   all */
  cJSON *clients_array = item(acl, "clients");
  if (clients_array && (!cJSON_IsArray(clients_array) ||
                        cJSON_GetArraySize(clients_array) > P_CLIENTS))
    return ESP_ERR_INVALID_STATE;
  if (!clients_array || !cJSON_GetArraySize(clients_array)) {
    disabled = true;
    return ESP_OK;
  }
  journal = gw_store_get_json(P_STORE);
  if (!journal) {
    size_t stored = 0;
    if (gw_store_get(P_STORE, NULL, &stored) != ESP_ERR_NOT_FOUND)
      return ESP_ERR_INVALID_STATE;
    journal = cJSON_CreateObject();
    if (!journal)
      return ESP_ERR_NO_MEM;
    if (!cJSON_AddNumberToObject(journal, "version", 1) ||
        !cJSON_AddStringToObject(journal, "namespaceId", namespace_id) ||
        !cJSON_AddStringToObject(journal, "gatewayId", gateway_id) ||
        !cJSON_AddArrayToObject(journal, "events") ||
        !cJSON_AddArrayToObject(journal, "ledger") ||
        !cJSON_AddArrayToObject(journal, "tasks") ||
        !cJSON_AddArrayToObject(journal, "volatileSessions") ||
        !cJSON_AddStringToObject(journal, "lowSequence", "0")) {
      cJSON_Delete(journal);
      journal = NULL;
      return ESP_ERR_NO_MEM;
    }
  }
  if (strcmp(str(journal, "namespaceId"), namespace_id) ||
      strcmp(str(journal, "gatewayId"), gateway_id) ||
      !cJSON_IsArray(item(journal, "events")) ||
      !cJSON_IsArray(item(journal, "tasks")) ||
      !cJSON_IsArray(item(journal, "ledger")))
    return ESP_ERR_INVALID_STATE;
  const cJSON *v;
  cJSON_ArrayForEach(v,
                     item(journal, "volatileSessions")) if (cJSON_IsString(v))
      count("sessionLost");
  cJSON *empty_sessions = cJSON_CreateArray();
  if (!empty_sessions ||
      !(item(journal, "volatileSessions")
            ? cJSON_ReplaceItemInObjectCaseSensitive(
                  journal, "volatileSessions", empty_sessions)
            : cJSON_AddItemToObject(journal, "volatileSessions",
                                    empty_sessions))) {
    cJSON_Delete(empty_sessions);
    return ESP_ERR_NO_MEM;
  }
  cJSON *t;
  cJSON_ArrayForEach(t, item(journal, "tasks")) if (
      !strcmp(str(t, "state"),
              "EXECUTING")) if (!setstr(t, "state",
                                        "UNKNOWN")) return ESP_ERR_NO_MEM;
  if (commit() != ESP_OK)
    return ESP_FAIL;
  ready = true;
  gw_ble_peripheral_set_receiver(local_receive, NULL);
  if (xTaskCreate(worker, "gw_peripheral", 12288, NULL, 4, NULL) != pdPASS) {
    ready = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
static bool ack_valid(const cJSON *ack, const cJSON *batch,
                      const char *event_id, const cJSON **result) {
  static const char *const fields[] = {"schemaVersion", "namespaceId",
                                       "gatewayId",     "batchId",
                                       "durability",    "items"};
  static const char *const item_fields[] = {"eventId", "status", "code"};
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(ack, "items");
  if (!keys(ack, fields, 6) || strcmp(str(ack, "schemaVersion"), "1.0") ||
      strcmp(str(ack, "namespaceId"), namespace_id) ||
      strcmp(str(ack, "gatewayId"), gateway_id) ||
      strcmp(str(ack, "batchId"), str(batch, "batchId")) ||
      strcmp(str(ack, "durability"), "PERIPHERAL_PERSISTED") ||
      !cJSON_IsArray(items) || cJSON_GetArraySize(items) != 1)
    return false;
  const cJSON *r = items->child;
  const char *status = str(r, "status"), *code = str(r, "code");
  if (!keys(r, item_fields, 3) || strcmp(str(r, "eventId"), event_id))
    return false;
  if (!strcmp(status, "ACCEPTED") || !strcmp(status, "DUPLICATE")) {
    if (cJSON_GetObjectItemCaseSensitive(r, "code"))
      return false;
  } else if (!strcmp(status, "RETRY")) {
    if (strcmp(code, "CAPACITY_TEMPORARY") &&
        strcmp(code, "INTERNAL_TEMPORARY"))
      return false;
  } else if (!strcmp(status, "REJECTED")) {
    if (strcmp(code, "NOT_AUTHORIZED") && strcmp(code, "INVALID_EVENT") &&
        strcmp(code, "EVENT_ID_CONFLICT") &&
        strcmp(code, "BUSINESS_ID_CONFLICT"))
      return false;
  } else
    return false;
  *result = r;
  return true;
}
esp_err_t gw_peripheral_service(const char *ns, const char *gw,
                                const char *interface, uint64_t deadline) {
  if (!ready || !ns || !gw || strcmp(ns, namespace_id) ||
      strcmp(gw, gateway_id))
    return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(upload_mutex, 0) != pdTRUE)
    return ESP_ERR_NOT_FINISHED;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *p = NULL, *v;
  if (gw_core_mono_ms() >= next_upload)
    cJSON_ArrayForEach(
        v, item(journal, "events")) if (!strcmp(str(v, "state"), "PENDING")) {
      p = v;
      break;
    }
  cJSON *batch = p ? cJSON_CreateObject() : NULL;
  bool batch_ok = p == NULL;
  char id[33] = {0};
  if (batch) {
    snprintf(id, sizeof(id), "%s", str(item(p, "event"), "eventId"));
    batch_ok = cJSON_AddStringToObject(batch, "schemaVersion", "1.0") &&
               cJSON_AddStringToObject(batch, "namespaceId", ns) &&
               cJSON_AddStringToObject(batch, "gatewayId", gw) &&
               cJSON_AddStringToObject(batch, "batchId", str(p, "batchId"));
    cJSON *events = batch_ok ? cJSON_AddArrayToObject(batch, "events") : NULL;
    cJSON *event = events ? cJSON_Duplicate(item(p, "event"), true) : NULL;
    batch_ok = event && cJSON_AddItemToArray(events, event);
    if (!batch_ok)
      cJSON_Delete(event);
  }
  xSemaphoreGive(mutex);
  if (!batch_ok || !batch) {
    cJSON_Delete(batch);
    xSemaphoreGive(upload_mutex);
    return batch_ok ? ESP_OK : ESP_ERR_NO_MEM;
  }
  cJSON *ack = NULL;
  esp_err_t e = gw_transport_json("/api/v1/gateway-peripheral/batches", "POST",
                                  batch, interface, deadline, &ack);
  const cJSON *receipt = NULL;
  bool valid = e == ESP_OK && ack_valid(ack, batch, id, &receipt);
  xSemaphoreTake(mutex, portMAX_DELAY);
  next_upload = gw_core_mono_ms() + 1000;
  if (valid) {
    int index = 0;
    cJSON_ArrayForEach(v, item(journal, "events")) {
      if (!strcmp(str(item(v, "event"), "eventId"), id)) {
        const char *status = str(receipt, "status");
        if (!strcmp(status, "ACCEPTED") || !strcmp(status, "DUPLICATE"))
          cJSON_DeleteItemFromArray(item(journal, "events"), index);
        else if (!strcmp(status, "REJECTED")) {
          cJSON *quarantined = cJSON_Duplicate(v, true);
          if (!quarantined || !setstr(quarantined, "state", "QUARANTINE") ||
              !setstr(quarantined, "reason", str(receipt, "code")) ||
              !cJSON_ReplaceItemViaPointer(item(journal, "events"), v,
                                           quarantined)) {
            cJSON_Delete(quarantined);
            e = ESP_ERR_NO_MEM;
            break;
          }
        }
        e = commit();
        break;
      }
      ++index;
    }
  } else if (e == ESP_OK)
    e = ESP_ERR_INVALID_RESPONSE;
  xSemaphoreGive(mutex);
  cJSON_Delete(ack);
  cJSON_Delete(batch);
  xSemaphoreGive(upload_mutex);
  return e;
}
static esp_err_t task_result(cJSON *task, cJSON **details) {
  const char *state = str(task, "state");
  cJSON *d = cJSON_CreateObject();
  if (!d)
    return ESP_ERR_NO_MEM;
  bool okay =
      cJSON_AddStringToObject(d, "radio", str(task, "radio")) &&
      cJSON_AddStringToObject(d, "clientId", str(task, "clientId")) &&
      cJSON_AddStringToObject(d, "connectionRef", str(task, "connectionRef"));
  if (*str(task, "businessId"))
    okay = okay &&
           cJSON_AddStringToObject(d, "businessId", str(task, "businessId"));
  if (!strcmp(state, "CLOSED"))
    okay = okay && cJSON_AddStringToObject(d, "connectionState", "CLOSED");
  if (*str(task, "businessStatus"))
    okay = okay && cJSON_AddStringToObject(d, "businessStatus",
                                           str(task, "businessStatus"));
  if (*str(task, "responseBase64"))
    okay = okay && cJSON_AddStringToObject(d, "responseBase64",
                                           str(task, "responseBase64"));
  okay = okay && cJSON_AddStringToObject(d, "verification",
                                         !strcmp(state, "APPLIED") ||
                                                 !strcmp(state, "CLOSED")
                                             ? "VERIFIED"
                                             : "UNKNOWN");
  if (!okay) {
    cJSON_Delete(d);
    return ESP_ERR_NO_MEM;
  }
  *details = d;
  if (!strcmp(state, "APPLIED") || !strcmp(state, "CLOSED"))
    return ESP_OK;
  if (!strcmp(state, "REJECTED"))
    return ESP_FAIL;
  return !strcmp(state, "EXECUTING") ? ESP_ERR_NOT_FINISHED : ESP_ERR_TIMEOUT;
}
static session_t *task_session(const cJSON *p) {
  const char *radio = str(p, "radio");
  if (strcmp(radio, "NRF54") && strcmp(radio, "ESP32"))
    return NULL;
  session_t *s = &sessions[!strcmp(radio, "ESP32")];
  return s->used && s->authenticated &&
                 !strcmp(s->reference, str(p, "connectionRef")) &&
                 !strcmp(s->client, str(p, "clientId")) &&
                 gw_core_mono_ms() < s->expires && live(s)
             ? s
             : NULL;
}
static bool task_link_closed(const cJSON *task) {
  uint8_t expected[16], boot[16];
  uint64_t connection;
  if (!unhex(str(task, "controllerBootId"), expected, 16) ||
      !number(task, "connectionId", UINT32_MAX, &connection))
    return false;
  if (!strcmp(str(task, "radio"), "ESP32")) {
    gw_capture_esp_boot(boot);
    return !memcmp(boot, expected, 16) &&
           !gw_ble_peripheral_identity((uint32_t)connection, NULL, NULL, NULL,
                                       NULL, NULL);
  }
  if (!gw_nrf_identity(boot, NULL, NULL) || memcmp(boot, expected, 16))
    return false;
  uint8_t status, state[16];
  size_t length = sizeof(state);
  return gw_nrf_command(0x0d, NULL, 0, &status, state, &length,
                        gw_core_mono_ms() + 250) == ESP_OK &&
         !status && length == 16 &&
         (state[4] == 0 || gw_get_u32(state) != (uint32_t)connection);
}
esp_err_t gw_peripheral_recover(const cJSON *command, uint64_t deadline,
                                cJSON **details) {
  (void)deadline;
  if (!ready || !gw_authority_has("PERIPHERAL_BRIDGE_1"))
    return ESP_ERR_NOT_SUPPORTED;
  if (!command || !details)
    return ESP_ERR_INVALID_ARG;
  *details = NULL;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *task = find_task(str(command, "taskId"));
  esp_err_t e =
      task ? (cJSON_Compare(
                  item(task, "parameters"),
                  cJSON_GetObjectItemCaseSensitive(command, "parameters"), true)
                  ? task_result(task, details)
                  : ESP_ERR_INVALID_STATE)
           : ESP_ERR_TIMEOUT;
  xSemaphoreGive(mutex);
  return e;
}
esp_err_t gw_peripheral_execute(const cJSON *command, uint64_t deadline,
                                cJSON **details) {
  if (!ready || !gw_authority_has("PERIPHERAL_BRIDGE_1"))
    return ESP_ERR_NOT_SUPPORTED;
  if (!command || !details)
    return ESP_ERR_INVALID_ARG;
  *details = NULL;
  const char *type = str(command, "type"), *task_id = str(command, "taskId");
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(command, "parameters");
  bool disconnect = !strcmp(type, "PERIPHERAL_DISCONNECT");
  if (!disconnect && strcmp(type, "PERIPHERAL_NOTIFY"))
    return ESP_ERR_NOT_SUPPORTED;
  static const char *const notify_fields[] = {
      "radio",      "connectionRef", "clientId",      "inReplyTo",
      "businessId", "route",         "payloadBase64", "timeoutMs"};
  static const char *const close_fields[] = {"radio", "connectionRef",
                                             "clientId"};
  if (!keys(p, disconnect ? close_fields : notify_fields, disconnect ? 3 : 8))
    return ESP_ERR_INVALID_ARG;
  if (!*task_id || strlen(task_id) > 128 || !cJSON_IsObject(p))
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *existing = find_task(task_id);
  if (existing) {
    esp_err_t e = cJSON_Compare(item(existing, "parameters"), p, true)
                      ? task_result(existing, details)
                      : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(mutex);
    return e;
  }
  session_t *s = task_session(p);
  uint64_t timeout = 0;
  if (!s || s->tx_pending ||
      cJSON_GetArraySize(item(journal, "tasks")) >= P_TASKS ||
      (!disconnect &&
       (!number(p, "timeoutMs", 30000, &timeout) || timeout < 100 ||
        !business_id(str(p, "businessId")) ||
        !route_allowed(s, str(p, "route")) ||
        !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(p, "payloadBase64")) ||
        !payload_valid(str(p, "payloadBase64")) ||
        (cJSON_GetObjectItemCaseSensitive(p, "inReplyTo") &&
         !business_id(str(p, "inReplyTo")))))) {
    xSemaphoreGive(mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (!disconnect) {
    uint64_t limit = gw_core_mono_ms() + timeout;
    if (deadline > limit)
      deadline = limit;
    cJSON *prior;
    cJSON_ArrayForEach(
        prior, item(journal, "tasks")) if (!strcmp(str(prior, "clientId"),
                                                   s->client) &&
                                           !strcmp(str(prior, "businessId"),
                                                   str(p, "businessId"))) {
      xSemaphoreGive(mutex);
      return ESP_ERR_INVALID_STATE;
    }
  }
  if (deadline <= gw_core_mono_ms()) {
    xSemaphoreGive(mutex);
    return ESP_ERR_TIMEOUT;
  }
  cJSON *task = cJSON_CreateObject();
  if (!task) {
    xSemaphoreGive(mutex);
    return ESP_ERR_NO_MEM;
  }
  char boot[33];
  hex(s->boot, 16, boot);
  bool okay =
      cJSON_AddStringToObject(task, "taskId", task_id) &&
      cJSON_AddStringToObject(task, "state", "EXECUTING") &&
      cJSON_AddStringToObject(task, "type", type) &&
      cJSON_AddStringToObject(task, "clientId", s->client) &&
      cJSON_AddStringToObject(task, "connectionRef", s->reference) &&
      cJSON_AddStringToObject(task, "radio", s->radio ? "ESP32" : "NRF54") &&
      cJSON_AddStringToObject(task, "controllerBootId", boot) &&
      cJSON_AddNumberToObject(task, "connectionId", s->connection) &&
      cJSON_AddNumberToObject(task, "deadlineMs", (double)deadline) &&
      cJSON_AddStringToObject(task, "businessId", str(p, "businessId")) &&
      cJSON_AddNumberToObject(task, "transportSeq", (double)s->tx_sequence + 1);
  cJSON *parameters = okay ? cJSON_Duplicate(p, true) : NULL;
  if (!parameters || !cJSON_AddItemToObject(task, "parameters", parameters)) {
    cJSON_Delete(parameters);
    cJSON_Delete(task);
    xSemaphoreGive(mutex);
    return ESP_ERR_NO_MEM;
  }
  if (!cJSON_AddItemToArray(item(journal, "tasks"), task)) {
    cJSON_Delete(task);
    xSemaphoreGive(mutex);
    return ESP_ERR_NO_MEM;
  }
  esp_err_t e = commit();
  if (e != ESP_OK) {
    xSemaphoreGive(mutex);
    return e;
  }
  if (disconnect)
    e = close_link(s);
  else {
    cJSON *message = cJSON_CreateObject();
    bool message_ok =
        message && cJSON_AddStringToObject(message, "schemaVersion", "1.0") &&
        cJSON_AddNumberToObject(message, "transportSeq",
                                (double)s->tx_sequence + 1);
    const char *names[] = {"businessId", "route", "payloadBase64", "inReplyTo"};
    for (size_t i = 0; i < 4; i++)
      if (cJSON_GetObjectItemCaseSensitive(p, names[i]))
        message_ok = message_ok && cJSON_AddStringToObject(message, names[i],
                                                           str(p, names[i]));
    e = message_ok ? queue_json(s, 1, message) : ESP_ERR_NO_MEM;
    if (e == ESP_OK && s->tx_deadline > deadline)
      s->tx_deadline = deadline;
    cJSON_Delete(message);
  }
  if (e != ESP_OK) {
    task = find_task(task_id);
    if (setstr(task, "state", "UNKNOWN"))
      (void)commit();
    xSemaphoreGive(mutex);
    return e == ESP_ERR_NO_MEM ? e : ESP_ERR_TIMEOUT;
  }
  xSemaphoreGive(mutex);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(mutex, portMAX_DELAY);
    task = find_task(task_id);
    s = task_session(p);
    if (disconnect && task_link_closed(task)) {
      e = setstr(task, "state", "CLOSED") ? commit() : ESP_ERR_NO_MEM;
    } else if ((!disconnect && !s) || gw_core_mono_ms() >= deadline) {
      e = !strcmp(str(task, "state"), "EXECUTING") &&
                  !setstr(task, "state", "UNKNOWN")
              ? ESP_ERR_NO_MEM
              : commit();
    } else
      e = ESP_OK;
    task = find_task(task_id);
    if (e != ESP_OK || strcmp(str(task, "state"), "EXECUTING")) {
      esp_err_t result = e == ESP_OK ? task_result(task, details) : e;
      xSemaphoreGive(mutex);
      return result;
    }
    xSemaphoreGive(mutex);
  }
}
cJSON *gw_peripheral_diagnostics(void) {
  if (!mutex)
    return NULL;
  xSemaphoreTake(mutex, portMAX_DELAY);
  cJSON *o = cJSON_CreateObject();
  if (o) {
    cJSON_AddBoolToObject(o, "ready", ready);
    cJSON_AddNumberToObject(o, "businessSpool",
                            cJSON_GetArraySize(item(journal, "events")));
    cJSON_AddNumberToObject(o, "businessLedger",
                            cJSON_GetArraySize(item(journal, "ledger")));
    cJSON_AddNumberToObject(o, "taskEvidence",
                            cJSON_GetArraySize(item(journal, "tasks")));
    cJSON_AddNumberToObject(o, "sessionLost",
                            cJSON_IsNumber(item(journal, "sessionLost"))
                                ? item(journal, "sessionLost")->valuedouble
                                : 0);
    cJSON_AddNumberToObject(
        o, "rejectedFragments",
        cJSON_IsNumber(item(journal, "rejectedFragments"))
            ? item(journal, "rejectedFragments")->valuedouble
            : 0);
  }
  xSemaphoreGive(mutex);
  return o;
}
