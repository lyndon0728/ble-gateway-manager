/* Production orchestration with fake transport, durable snapshot and SDK crypto
 * boundaries. This does not replace the SDK SRP/AES vectors or board BLE tests.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <time.h>
#ifdef _WIN32
static struct tm *gmtime_r(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) == 0 ? out : NULL;
}
#endif
#include "../gw_peripheral.c"
#include <assert.h>
#include <stdio.h>

static uint64_t now = 1000;
static cJSON *saved, *inbox;
static bool store_fail, ack_lost, transport_lost, authorized = true;
static uint32_t nrf_id = 11, esp_id = 22;
static uint8_t nrf_state = 1;
static bool esp_connected = true;
static unsigned store_calls, rx_acks, http_calls, closed_calls;
static unsigned physical_calls;
static bool require_intent;
static int allocation_fail_after = -1;
static bool allocation_failed;
static void *fault_malloc(size_t n) {
  if (allocation_fail_after == 0) {
    allocation_fail_after = -1;
    allocation_failed = true;
    return NULL;
  }
  if (allocation_fail_after > 0)
    --allocation_fail_after;
  return malloc(n);
}
static void inject_oom(unsigned index) {
  cJSON_Hooks hooks = {.malloc_fn = fault_malloc, .free_fn = free};
  allocation_fail_after = (int)index;
  allocation_failed = false;
  cJSON_InitHooks(&hooks);
}
static bool stop_oom(void) {
  cJSON_InitHooks(NULL);
  allocation_fail_after = -1;
  return allocation_failed;
}
static void assert_complete_intent(void) {
  if (!require_intent)
    return;
  cJSON *task = item(saved, "tasks")->child;
  assert(task && !task->next);
  const char *names[] = {"taskId",          "state",         "type",
                         "clientId",        "connectionRef", "radio",
                         "controllerBootId"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    assert(*str(task, names[i]));
  assert(cJSON_IsNumber(item(task, "connectionId")) &&
         cJSON_IsNumber(item(task, "deadlineMs")) &&
         cJSON_IsNumber(item(task, "transportSeq")));
  cJSON *p = item(task, "parameters");
  assert(cJSON_IsObject(p) && !strcmp(str(p, "radio"), "NRF54") &&
         !strcmp(str(p, "clientId"), "phone") &&
         !strcmp(str(p, "connectionRef"), str(task, "connectionRef")));
  if (!strcmp(str(task, "type"), "PERIPHERAL_NOTIFY"))
    assert(business_id(str(p, "businessId")) &&
           !strcmp(str(p, "businessId"), str(task, "businessId")) &&
           !strcmp(str(p, "route"), "telemetry") &&
           !strcmp(str(p, "payloadBase64"), "AQ==") &&
           item(p, "timeoutMs")->valueint == 100);
}
static const char *http_status = "ACCEPTED", *http_code = "";
static unsigned phone_mode, phone_ticks;
static uint64_t last_low;
static char last_batch[33], last_event[33];
static gw_ble_peripheral_receiver receive_local;
typedef struct {
  unsigned cap, size, head, count;
  uint8_t *bytes;
} mock_queue_t;
uint64_t gw_core_mono_ms(void) { return now; }
bool gw_core_time_bounds(uint64_t *lower, uint64_t *upper) {
  (void)lower;
  (void)upper;
  return false;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) {
  (void)t;
  return s != NULL;
}
void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }
QueueHandle_t xQueueCreate(unsigned cap, unsigned size) {
  mock_queue_t *q = calloc(1, sizeof(*q));
  q->cap = cap;
  q->size = size;
  q->bytes = calloc(cap, size);
  return q;
}
int xQueueSend(QueueHandle_t h, const void *p, unsigned wait) {
  (void)wait;
  mock_queue_t *q = h;
  if (q->count == q->cap)
    return 0;
  memcpy(q->bytes + ((q->head + q->count) % q->cap) * q->size, p, q->size);
  q->count++;
  return 1;
}
int xQueueReceive(QueueHandle_t h, void *p, unsigned wait) {
  (void)wait;
  mock_queue_t *q = h;
  if (!q->count)
    return 0;
  memcpy(p, q->bytes + q->head * q->size, q->size);
  q->head = (q->head + 1) % q->cap;
  q->count--;
  return 1;
}
int xTaskCreate(void (*f)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, void *handle) {
  (void)f;
  (void)name;
  (void)stack;
  (void)arg;
  (void)priority;
  (void)handle;
  return 1;
}
bool gw_authority_has(const char *s) {
  assert(!strcmp(s, "PERIPHERAL_BRIDGE_1"));
  return authorized;
}
bool gw_authority_board_verified(const char *s) {
  assert(strstr(s, "BLE_PERIPHERAL"));
  return authorized;
}
esp_err_t gw_factory_get_string(const char *key, char *out, size_t n) {
  const char *value =
      !strcmp(key, "namespace") ? "tenant"
      : !strcmp(key, "gateway") ? "gateway"
      : !strcmp(key, "periph_acl")
          ? "{\"clients\":[{\"clientId\":\"phone\",\"username\":\"business-"
            "phone\",\"slot\":0,\"enabled\":true,\"routes\":[\"telemetry\"]}]}"
          : NULL;
  if (!value)
    return ESP_ERR_NOT_FOUND;
  if (strlen(value) >= n)
    return ESP_ERR_INVALID_SIZE;
  strcpy(out, value);
  return ESP_OK;
}
esp_err_t gw_factory_get_blob(const char *key, void *out, size_t *n) {
  assert(!strncmp(key, "periph0_", 8));
  memset(out, 0x5a, *n);
  return ESP_OK;
}
esp_err_t gw_store_put_json(const char *key, const cJSON *o) {
  assert(!strcmp(key, P_STORE));
  store_calls++;
  if (store_fail) {
    store_fail = false;
    return ESP_ERR_NO_MEM;
  }
  cJSON *replacement = cJSON_Duplicate(o, true);
  if (!replacement)
    return ESP_ERR_NO_MEM;
  cJSON_Delete(saved);
  saved = replacement;
  return ESP_OK;
}
cJSON *gw_store_get_json(const char *key) {
  assert(!strcmp(key, P_STORE));
  return cJSON_Duplicate(saved, true);
}
esp_err_t gw_store_get(const char *key, void *p, size_t *n) {
  (void)key;
  (void)p;
  (void)n;
  return saved ? ESP_OK : ESP_ERR_NOT_FOUND;
}
cJSON *gw_capture_peripheral_peek(void) { return cJSON_Duplicate(inbox, true); }
esp_err_t gw_capture_peripheral_ack(const char *token) {
  rx_acks++;
  if (ack_lost)
    return ESP_FAIL;
  if (inbox && !strcmp(token, str(inbox, "receiptToken"))) {
    cJSON_Delete(inbox);
    inbox = NULL;
  }
  return ESP_OK;
}
void gw_capture_esp_boot(uint8_t boot[16]) { memset(boot, 0x55, 16); }
bool gw_nrf_identity(uint8_t boot[16], uint32_t *caps, char version[64]) {
  if (boot)
    memset(boot, 0x44, 16);
  if (caps)
    *caps = 1u << 11;
  if (version)
    strcpy(version, "test");
  return true;
}
esp_err_t gw_nrf_command(uint8_t op, const void *arg, size_t n, uint8_t *status,
                         void *out, size_t *length, int64_t deadline) {
  (void)deadline;
  *status = 0;
  if (op == 0x0d) {
    assert(*length >= 16);
    uint8_t *p = out;
    memset(p, 0, 16);
    gw_put_u32(p, nrf_id);
    p[4] = nrf_state;
    gw_put_u16(p + 12, 247);
    p[14] = 1;
    *length = 16;
    return ESP_OK;
  }
  if (op == 0x0f) {
    assert(n == 4);
    assert_complete_intent();
    ++physical_calls;
    closed_calls++;
    nrf_state = 2;
    return ESP_OK;
  }
  assert(op == 0x0e && n >= 15);
  assert_complete_intent();
  ++physical_calls;
  uint64_t sequence = gw_get_u64(arg);
  assert(sequence > last_low);
  last_low = sequence;
  gw_put_u64(out, sequence);
  *length = 8;
  return ESP_OK;
}
void gw_ble_peripheral_set_receiver(gw_ble_peripheral_receiver fn, void *ctx) {
  (void)ctx;
  receive_local = fn;
}
bool gw_ble_peripheral_identity(uint32_t id, uint8_t *boot, uint8_t *address,
                                uint8_t *type, uint8_t *level, uint16_t *mtu) {
  if (!esp_connected || id != esp_id)
    return false;
  if (boot)
    memset(boot, 0x55, 16);
  if (address)
    memset(address, 0, 6);
  if (type)
    *type = 0;
  if (level)
    *level = 1;
  if (mtu)
    *mtu = 247;
  return true;
}
esp_err_t gw_ble_peripheral_send(uint32_t id, uint64_t seq, const uint8_t *p,
                                 size_t n) {
  assert(id == esp_id && seq > last_low && p && n > 12);
  last_low = seq;
  return ESP_OK;
}
esp_err_t gw_ble_peripheral_close(uint32_t id) {
  assert(id == esp_id);
  closed_calls++;
  esp_connected = false;
  return ESP_OK;
}
esp_err_t gw_transport_json(const char *path, const char *method,
                            const cJSON *batch, const char *iface,
                            uint64_t deadline, cJSON **out) {
  (void)iface;
  (void)deadline;
  assert(!strcmp(path, "/api/v1/gateway-peripheral/batches") &&
         !strcmp(method, "POST"));
  http_calls++;
  cJSON *event = item((cJSON *)batch, "events")->child;
  assert(event && !item(event, "trust"));
  if (last_batch[0]) {
    assert(!strcmp(last_batch, str(batch, "batchId")));
    assert(!strcmp(last_event, str(event, "eventId")));
  }
  snprintf(last_batch, 33, "%s", str(batch, "batchId"));
  snprintf(last_event, 33, "%s", str(event, "eventId"));
  if (transport_lost)
    return ESP_ERR_TIMEOUT;
  *out = cJSON_CreateObject();
  const char *keys_[] = {"schemaVersion", "namespaceId", "gatewayId",
                         "batchId"};
  for (unsigned i = 0; i < 4; i++)
    cJSON_AddStringToObject(*out, keys_[i], str(batch, keys_[i]));
  cJSON_AddStringToObject(*out, "durability", "PERIPHERAL_PERSISTED");
  cJSON *a = cJSON_AddArrayToObject(*out, "items"), *r = cJSON_CreateObject();
  cJSON_AddItemToArray(a, r);
  cJSON_AddStringToObject(r, "eventId", str(event, "eventId"));
  cJSON_AddStringToObject(r, "status", http_status);
  if (*http_code)
    cJSON_AddStringToObject(r, "code", http_code);
  return ESP_OK;
}
void esp_fill_random(void *p, size_t n) {
  static uint32_t x = 1;
  uint8_t *b = p;
  for (size_t i = 0; i < n; i++) {
    x = x * 1664525 + 1013904223;
    b[i] = (uint8_t)(x >> 24);
  }
}
uint16_t gw_get_u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
uint32_t gw_get_u32(const uint8_t *p) {
  return gw_get_u16(p) | (uint32_t)gw_get_u16(p + 2) << 16;
}
uint64_t gw_get_u64(const uint8_t *p) {
  return gw_get_u32(p) | (uint64_t)gw_get_u32(p + 4) << 32;
}
void gw_put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void gw_put_u32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
void gw_put_u64(uint8_t *p, uint64_t v) {
  gw_put_u32(p, (uint32_t)v);
  gw_put_u32(p + 4, (uint32_t)(v >> 32));
}
void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); }
int mbedtls_sha256(const unsigned char *p, size_t n, unsigned char out[32],
                   int variant) {
  (void)variant;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 16777619u;
  for (size_t i = 0; i < 32; i++) {
    h = h * 1664525u + 1013904223u;
    out[i] = (uint8_t)(h >> 24);
  }
  return 0;
}
static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *written,
                          const unsigned char *p, size_t n) {
  size_t total = ((n + 2) / 3) * 4;
  if (cap < total)
    return -1;
  size_t pos = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16;
    if (i + 1 < n)
      v |= (uint32_t)p[i + 1] << 8;
    if (i + 2 < n)
      v |= p[i + 2];
    out[pos++] = alphabet[(v >> 18) & 63];
    out[pos++] = alphabet[(v >> 12) & 63];
    out[pos++] = i + 1 < n ? alphabet[(v >> 6) & 63] : '=';
    out[pos++] = i + 2 < n ? alphabet[v & 63] : '=';
  }
  *written = pos;
  return 0;
}
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *written,
                          const unsigned char *p, size_t n) {
  if (n % 4)
    return -1;
  size_t pos = 0;
  for (size_t i = 0; i < n; i += 4) {
    uint32_t v = 0;
    unsigned pad = 0;
    for (unsigned j = 0; j < 4; j++) {
      if (p[i + j] == '=') {
        pad++;
        v <<= 6;
      } else {
        const char *q = strchr(alphabet, p[i + j]);
        if (!q || pad)
          return -1;
        v = (v << 6) | (unsigned)(q - alphabet);
      }
    }
    if (pad > 2 || pos + 3 - pad > cap)
      return -1;
    out[pos++] = (uint8_t)(v >> 16);
    if (pad < 2)
      out[pos++] = (uint8_t)(v >> 8);
    if (!pad)
      out[pos++] = (uint8_t)v;
  }
  *written = pos;
  return 0;
}
typedef struct {
  bool authenticated;
} crypto_t;
static esp_err_t security_init(void **h) {
  *h = calloc(1, sizeof(crypto_t));
  return *h ? ESP_OK : ESP_ERR_NO_MEM;
}
static esp_err_t security_cleanup(void *h) {
  free(h);
  return ESP_OK;
}
static esp_err_t security_session(void *h, uint32_t id) {
  return h && id ? ESP_OK : ESP_FAIL;
}
static esp_err_t security_handshake(void *h, const void *params, uint32_t id,
                                    const uint8_t *p, ssize_t n, uint8_t **out,
                                    ssize_t *length, void *unused) {
  (void)params;
  (void)id;
  (void)unused;
  if (!n || p[0] > 1)
    return ESP_FAIL;
  if (p[0] == 1)
    ((crypto_t *)h)->authenticated = true;
  *out = malloc(1);
  **out = 0x42;
  *length = 1;
  return ESP_OK;
}
static esp_err_t security_encrypt(void *h, uint32_t id, const uint8_t *p,
                                  ssize_t n, uint8_t **out, ssize_t *length) {
  (void)id;
  if (!((crypto_t *)h)->authenticated)
    return ESP_FAIL;
  *out = malloc(n + 1);
  (*out)[0] = 0xee;
  memcpy(*out + 1, p, n);
  *length = n + 1;
  return ESP_OK;
}
static esp_err_t security_decrypt(void *h, uint32_t id, const uint8_t *p,
                                  ssize_t n, uint8_t **out, ssize_t *length) {
  (void)id;
  if (!((crypto_t *)h)->authenticated || n < 2 || p[0] != 0xee)
    return ESP_FAIL;
  *out = malloc(n - 1);
  memcpy(*out, p + 1, n - 1);
  *length = n - 1;
  return ESP_OK;
}
const protocomm_security_t protocomm_security2 = {
    .ver = 2,
    .patch_ver = 1,
    .init = security_init,
    .cleanup = security_cleanup,
    .new_transport_session = security_session,
    .close_transport_session = security_session,
    .security_req_handler = security_handshake,
    .encrypt = security_encrypt,
    .decrypt = security_decrypt};
SessionData *session_data__unpack(void *allocator, size_t n, const uint8_t *p) {
  (void)allocator;
  if (!n || p[0] > 1)
    return NULL;
  SessionData *r = calloc(1, sizeof(*r));
  r->sec_ver = 2;
  r->proto_case = 12;
  r->sec2 = calloc(1, sizeof(*r->sec2));
  if (!p[0]) {
    r->sec2->msg = 0;
    r->sec2->payload_case = 20;
    r->sec2->sc0 = calloc(1, sizeof(Sec2Command0));
    r->sec2->sc0->client_pubkey.len = 384;
    r->sec2->sc0->client_username = (Blob){n - 1, (uint8_t *)p + 1};
  } else {
    r->sec2->msg = 2;
    r->sec2->payload_case = 22;
    r->sec2->sc1 = calloc(1, sizeof(Sec2Command1));
    r->sec2->sc1->client_proof.len = 64;
  }
  return r;
}
void session_data__free_unpacked(SessionData *r, void *a) {
  (void)a;
  free(r->sec2->sc0);
  free(r->sec2->sc1);
  free(r->sec2);
  free(r);
}
static void packet(session_t *s, uint8_t type, const uint8_t *message,
                   size_t n) {
  uint32_t seq = s->rx.next_sequence;
  size_t off = 0;
  uint8_t p[512];
  while (off < n) {
    size_t len =
        gw_pg_fragment(p, sizeof(p), type, seq, message, n, off, s->mtu);
    assert(len);
    consume(s, p, len, NULL, now);
    off += len - 12;
  }
}
static void drain(session_t *s) {
  while (s->used && s->tx_pending) {
    pump_tx(s);
    now++;
  }
}
static session_t *authenticate(uint8_t radio) {
  uint8_t boot[16];
  memset(boot, radio ? 0x55 : 0x44, 16);
  session_t *s = session_get(radio, boot, radio ? esp_id : nrf_id);
  assert(s);
  static const uint8_t first[] = {0,   'b', 'u', 's', 'i', 'n', 'e', 's',
                                  's', '-', 'p', 'h', 'o', 'n', 'e'};
  packet(s, 0, first, sizeof(first));
  assert(s->security);
  drain(s);
  uint8_t second = 1;
  packet(s, 0, &second, 1);
  assert(s->authenticated);
  drain(s);
  return s;
}
static void send_json(session_t *s, uint8_t type, cJSON *o) {
  char *plain = cJSON_PrintUnformatted(o);
  size_t n = strlen(plain);
  uint8_t *cipher = malloc(n + 1);
  cipher[0] = 0xee;
  memcpy(cipher + 1, plain, n);
  packet(s, type, cipher, n + 1);
  free(cipher);
  free(plain);
}
static void send_business(session_t *s, const char *id, const char *payload) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "schemaVersion", "1.0");
  cJSON_AddNumberToObject(o, "transportSeq", s->rx.next_sequence);
  cJSON_AddStringToObject(o, "businessId", id);
  cJSON_AddStringToObject(o, "route", "telemetry");
  cJSON_AddStringToObject(o, "payloadBase64", payload);
  send_json(s, 1, o);
  cJSON_Delete(o);
}
void vTaskDelay(unsigned ms) {
  now += ms;
  process_inputs();
  if (phone_mode) {
    session_t *s = &sessions[0];
    phone_ticks++;
    if (phone_mode == 3 && phone_ticks >= 3)
      nrf_state = 0;
    if (s->used && s->authenticated && s->tx_type == 1 && !s->tx_pending &&
        phone_ticks <= 2) {
      cJSON *task = item(journal, "tasks")->child;
      while (task && task->next)
        task = task->next;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "schemaVersion", "1.0");
      cJSON_AddNumberToObject(o, "transportSeq", s->rx.next_sequence);
      cJSON_AddStringToObject(o, "inReplyTo", str(task, "businessId"));
      cJSON_AddStringToObject(o, "status",
                              phone_mode == 1 && phone_ticks >= 2 ? "APPLIED"
                                                                  : "STORED");
      send_json(s, 2, o);
      cJSON_Delete(o);
    }
  }
}
static void reset(void) {
  (void)stop_oom();
  require_intent = false;
  for (unsigned i = 0; i < 2; i++)
    session_drop(&sessions[i]);
  cJSON_Delete(journal);
  journal = NULL;
  cJSON_Delete(acl);
  acl = NULL;
  cJSON_Delete(saved);
  saved = NULL;
  cJSON_Delete(inbox);
  inbox = NULL;
  if (local_inputs) {
    mock_queue_t *q = local_inputs;
    free(q->bytes);
    free(q);
  }
  local_inputs = NULL;
  mutex = upload_mutex = NULL;
  ready = disabled = false;
  receive_local = NULL;
  now = 1000;
  nrf_state = 1;
  nrf_id = 11;
  esp_id = 22;
  esp_connected = true;
  authorized = true;
  store_fail = ack_lost = transport_lost = false;
  store_calls = rx_acks = http_calls = closed_calls = phone_mode = phone_ticks =
      0;
  physical_calls = 0;
  last_low = next_upload = 0;
  last_batch[0] = last_event[0] = 0;
  http_status = "ACCEPTED";
  http_code = "";
  assert(gw_peripheral_init() == ESP_OK && ready && receive_local);
}
static void test_business_handoff(void) {
  reset();
  session_t *s = authenticate(0), *other = authenticate(1);
  assert(s->authenticated && other->authenticated &&
         s->security != other->security);
  send_business(s, "00112233445566778899aabbccddeeff", "AQID");
  assert(cJSON_GetArraySize(item(saved, "events")) == 1 &&
         cJSON_GetArraySize(item(saved, "ledger")) == 1);
  assert(s->tx_type == 2 && s->tx_pending);
  cJSON *ack = parse_json(s->tx + 1, s->tx_length - 1);
  assert(!strcmp(str(ack, "status"), "STORED"));
  cJSON_Delete(ack);
  drain(s);
  session_drop(s);
  nrf_id++;
  s = authenticate(0);
  send_business(s, "00112233445566778899aabbccddeeff", "AQID");
  assert(cJSON_GetArraySize(item(saved, "events")) == 1);
  drain(s);
  transport_lost = true;
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
             ESP_ERR_TIMEOUT &&
         cJSON_GetArraySize(item(saved, "events")) == 1);
  now += 1001;
  transport_lost = false;
  http_status = "DUPLICATE";
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
             ESP_OK &&
         cJSON_GetArraySize(item(saved, "events")) == 0);
  send_business(s, "00112233445566778899aabbccddeeff", "BA==");
  assert(!s->used && cJSON_GetArraySize(item(saved, "events")) == 0);
}
static void test_storage_and_session_loss(void) {
  reset();
  session_t *s = authenticate(0);
  store_fail = true;
  send_business(s, "10112233445566778899aabbccddeeff", "AQ==");
  assert(!s->used && cJSON_GetArraySize(item(saved, "events")) == 0);
  nrf_state = 1;
  nrf_id++;
  s = authenticate(0);
  send_business(s, "10112233445566778899aabbccddeeff", "AQ==");
  drain(s);
  http_status = "REJECTED";
  http_code = "NOT_AUTHORIZED";
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
         ESP_OK);
  assert(!strcmp(str(item(saved, "events")->child, "state"), "QUARANTINE"));
  cJSON *old = cJSON_Duplicate(saved, true);
  for (unsigned i = 0; i < 2; i++)
    session_drop(&sessions[i]);
  cJSON_Delete(journal);
  journal = cJSON_Duplicate(old, true);
  cJSON_Delete(old);
  /* Persistent business spool survives without session keys, but new cipher
   * messages without an initial handshake never get authenticated. */
  uint8_t boot[16];
  memset(boot, 0x44, 16);
  s = session_get(0, boot, nrf_id);
  assert(s && !s->authenticated);
  uint8_t bad[] = {0xee, '{', '}'};
  packet(s, 1, bad, sizeof(bad));
  assert(!s->used);
  assert(cJSON_GetArraySize(item(saved, "events")) == 1);
}
static cJSON *notify(session_t *s, const char *task, const char *business,
                     const char *type) {
  cJSON *c = cJSON_CreateObject(),
        *p = cJSON_AddObjectToObject(c, "parameters");
  cJSON_AddStringToObject(c, "taskId", task);
  cJSON_AddStringToObject(c, "type", type);
  cJSON_AddStringToObject(p, "radio", "NRF54");
  cJSON_AddStringToObject(p, "connectionRef", s->reference);
  cJSON_AddStringToObject(p, "clientId", "phone");
  if (!strcmp(type, "PERIPHERAL_NOTIFY")) {
    cJSON_AddStringToObject(p, "businessId", business);
    cJSON_AddStringToObject(p, "route", "telemetry");
    cJSON_AddStringToObject(p, "payloadBase64", "AQ==");
    cJSON_AddNumberToObject(p, "timeoutMs", 100);
  }
  return c;
}
static void test_downlink_proof(void) {
  reset();
  session_t *s = authenticate(0);
  cJSON *c = notify(s, "task1", "20112233445566778899aabbccddeeff",
                    "PERIPHERAL_NOTIFY"),
        *d = NULL;
  phone_mode = 1;
  assert(gw_peripheral_execute(c, now + 1000, &d) == ESP_OK);
  assert(phone_ticks >= 2 && !strcmp(str(d, "businessStatus"), "APPLIED") &&
         !strcmp(str(d, "verification"), "VERIFIED"));
  cJSON_Delete(d);
  unsigned writes = store_calls;
  assert(gw_peripheral_recover(c, now + 1000, &d) == ESP_OK &&
         store_calls == writes);
  cJSON_Delete(d);
  cJSON_Delete(c);
  c = notify(s, "task2", "30112233445566778899aabbccddeeff",
             "PERIPHERAL_NOTIFY");
  phone_mode = 2;
  phone_ticks = 0;
  assert(gw_peripheral_execute(c, now + 1000, &d) == ESP_ERR_TIMEOUT);
  assert(!strcmp(str(d, "businessStatus"), "STORED"));
  cJSON_Delete(d);
  cJSON_Delete(c);
  c = notify(s, "task3", NULL, "PERIPHERAL_DISCONNECT");
  phone_mode = 3;
  phone_ticks = 0;
  assert(gw_peripheral_execute(c, now + 1000, &d) == ESP_OK &&
         phone_ticks >= 3 && !strcmp(str(d, "connectionState"), "CLOSED"));
  cJSON_Delete(d);
  cJSON_Delete(c);
}
static void test_pg_boundaries(void) {
  gw_pg_rx_t r;
  gw_pg_init(&r);
  uint8_t data[4096], fragment[512];
  memset(data, 0x5a, sizeof(data));
  size_t n = gw_pg_fragment(fragment, sizeof(fragment), 0, 1, data,
                            sizeof(data), 0, 23);
  assert(n == 20);
  assert(gw_pg_receive(&r, fragment, n, 23, 100) == GW_PG_MORE);
  assert(gw_pg_receive(&r, fragment, n, 23, 101) == GW_PG_DUPLICATE);
  fragment[12] ^= 1;
  assert(gw_pg_receive(&r, fragment, n, 23, 102) == GW_PG_INVALID);
  fragment[12] ^= 1;
  n = gw_pg_fragment(fragment, sizeof(fragment), 0, 1, data, sizeof(data), 8,
                     23);
  assert(gw_pg_receive(&r, fragment, n, 23, 5099) == GW_PG_MORE);
  n = gw_pg_fragment(fragment, sizeof(fragment), 0, 1, data, sizeof(data), 16,
                     23);
  assert(gw_pg_receive(&r, fragment, n, 23, 5100) == GW_PG_EXPIRED);
  gw_pg_init(&r);
  n = gw_pg_fragment(fragment, sizeof(fragment), 0, 1, data, 5, 0, 247);
  assert(gw_pg_receive(&r, fragment, n, 247, 1) == GW_PG_COMPLETE);
  assert(gw_pg_receive(&r, fragment, n, 247, 2) == GW_PG_DUPLICATE);
  fragment[4] = 3;
  assert(gw_pg_receive(&r, fragment, n, 247, 3) == GW_PG_INVALID);
  assert(!gw_pg_fragment(fragment, sizeof(fragment), 0, 1, data, 4097, 0, 247));
  gw_pg_init(&r);
  size_t offset = 0;
  while (offset < sizeof(data)) {
    n = gw_pg_fragment(fragment, sizeof(fragment), 1, 1, data, sizeof(data),
                       offset, 247);
    int result = gw_pg_receive(&r, fragment, n, 247, 100 + offset / 200);
    offset += n - 12;
    assert(result == (offset == sizeof(data) ? GW_PG_COMPLETE : GW_PG_MORE));
  }
  assert(!memcmp(r.bytes, data, sizeof(data)));
}
static void test_unauthorized_and_parser(void) {
  reset();
  uint8_t boot[16];
  memset(boot, 0x44, 16);
  session_t *s = session_get(0, boot, nrf_id);
  assert(s);
  const uint8_t admin[] = {0, 'a', 'd', 'm', 'i', 'n'};
  packet(s, 0, admin, sizeof(admin));
  assert(!s->used && !cJSON_GetArraySize(item(saved, "events")));
  nrf_state = 1;
  nrf_id++;
  s = session_get(0, boot, nrf_id);
  assert(s);
  const uint8_t plain[] = {0xee, '{', '}'};
  packet(s, 1, plain, sizeof(plain));
  assert(!s->used && !cJSON_GetArraySize(item(saved, "events")));
  const char nul[] = "{\"businessId\":\"abc\\u0000def\"}";
  assert(!parse_json((const uint8_t *)nul, strlen(nul)));
  const char deep[] = "[[[[[[]]]]]]";
  assert(!parse_json((const uint8_t *)deep, strlen(deep)));
  authorized = false;
  assert(!session_get(0, boot, nrf_id));
  cJSON *details = NULL, *command_ = cJSON_CreateObject();
  assert(gw_peripheral_execute(command_, now + 100, &details) ==
             ESP_ERR_NOT_SUPPORTED &&
         !details);
  cJSON_Delete(command_);
}
static void put_inbox(const uint8_t *fragment, size_t n, const char *token,
                      bool old_boot) {
  cJSON_Delete(inbox);
  inbox = cJSON_CreateObject();
  char payload[700], boot[33];
  size_t encoded = 0;
  uint8_t raw[16];
  assert(!mbedtls_base64_encode((unsigned char *)payload, sizeof(payload),
                                &encoded, fragment, n));
  payload[encoded] = 0;
  cJSON_AddStringToObject(inbox, "receiptToken", token);
  cJSON_AddStringToObject(inbox, "payloadBase64", payload);
  cJSON_AddStringToObject(inbox, "radio", "NRF54");
  memset(raw, 0x44, 16);
  hex(raw, 16, boot);
  cJSON_AddStringToObject(inbox, "controllerBootId", boot);
  memset(raw, old_boot ? 0x66 : 0x55, 16);
  hex(raw, 16, boot);
  cJSON_AddStringToObject(inbox, "ingressBootId", boot);
  cJSON_AddNumberToObject(inbox, "connectionId", nrf_id);
  cJSON_AddNumberToObject(inbox, "arrivedMs", now);
}
static void test_inbox_transaction_and_limits(void) {
  reset();
  uint8_t fragment[512], first[] = {0,   'b', 'u', 's', 'i', 'n', 'e', 's',
                                    's', '-', 'p', 'h', 'o', 'n', 'e'};
  size_t n = gw_pg_fragment(fragment, sizeof(fragment), 0, 1, first,
                            sizeof(first), 0, 247);
  put_inbox(fragment, n, "old-boot", true);
  process_inputs();
  assert(!inbox && !sessions[0].used &&
         item(saved, "sessionLost")->valueint == 1);
  put_inbox(fragment, n, "one", false);
  ack_lost = true;
  process_inputs();
  assert(inbox && sessions[0].security && rx_acks > 1);
  unsigned commits = store_calls;
  ack_lost = false;
  process_inputs();
  assert(!inbox && store_calls == commits); /* durable receipt skip */
  session_drop(&sessions[0]);
  nrf_id++;
  nrf_state = 1;
  session_t *s = authenticate(0);
  for (unsigned i = 0; i < 8; i++) {
    char id[33];
    snprintf(id, sizeof(id), "%032x", i + 1);
    send_business(s, id, "AQ==");
    drain(s);
  }
  assert(cJSON_GetArraySize(item(saved, "events")) == 8);
  send_business(s, "ffffffffffffffffffffffffffffffff", "AQ==");
  assert(!s->used && cJSON_GetArraySize(item(saved, "events")) == 8);
  /* Invalid success ACK must never release an event; permanent rejection is
   * independently persisted as quarantine, not reported as business success. */
  http_code = "INVALID_EVENT";
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
         ESP_ERR_INVALID_RESPONSE);
  assert(cJSON_GetArraySize(item(saved, "events")) == 8);
  now += 1001;
  http_code = "";
  store_fail = true;
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
         ESP_ERR_NO_MEM);
  assert(cJSON_GetArraySize(item(saved, "events")) == 8);
  now += 1001;
  http_status = "DUPLICATE";
  assert(gw_peripheral_service("tenant", "gateway", NULL, now + 1000) ==
         ESP_OK);
  assert(cJSON_GetArraySize(item(saved, "events")) == 7);
  /* Queue admission is bounded even before application authentication. */
  for (unsigned i = 0; i < 16; i++)
    assert(receive_local(esp_id, fragment, n, 247, NULL) == ESP_OK);
  assert(receive_local(esp_id, fragment, n, 247, NULL) == ESP_ERR_NO_MEM);
}
static cJSON *business_request(session_t *s) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "schemaVersion", "1.0");
  cJSON_AddNumberToObject(o, "transportSeq", s->rx.next_sequence);
  cJSON_AddStringToObject(o, "businessId", "00112233445566778899aabbccddeeff");
  cJSON_AddStringToObject(o, "inReplyTo", "ffeeddccbbaa99887766554433221100");
  cJSON_AddStringToObject(o, "route", "telemetry");
  cJSON_AddStringToObject(o, "payloadBase64", "AQID");
  return o;
}
static void assert_complete_business(cJSON *state) {
  cJSON *events = item(state, "events"), *ledger = item(state, "ledger");
  assert(cJSON_GetArraySize(events) == cJSON_GetArraySize(ledger));
  if (!events->child)
    return;
  assert(!events->child->next && !ledger->child->next);
  cJSON *spool = events->child, *event = item(spool, "event"),
        *entry = ledger->child;
  assert(business_id(str(spool, "batchId")) &&
         !strcmp(str(spool, "state"), "PENDING"));
  const char *required[] = {
      "eventId",    "radio", "controllerBootId", "connectionRef", "clientId",
      "businessId", "route", "payloadBase64",    "timeQuality",   "inReplyTo"};
  for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i)
    assert(*str(event, required[i]));
  assert(
      !strcmp(str(event, "payloadBase64"), "AQID") &&
      !strcmp(str(event, "route"), "telemetry") &&
      !strcmp(str(event, "businessId"), "00112233445566778899aabbccddeeff") &&
      !strcmp(str(event, "inReplyTo"), "ffeeddccbbaa99887766554433221100") &&
      cJSON_IsNumber(item(event, "transportSeq")) &&
      !strcmp(str(entry, "clientId"), "phone") &&
      !strcmp(str(entry, "businessId"), str(event, "businessId")) &&
      strlen(str(entry, "digest")) == 64);
}
static void test_business_oom(void) {
  unsigned index;
  for (index = 0; index < 1600; ++index) {
    reset();
    session_t *s = authenticate(0);
    s->received_utc_ms = UINT64_C(1700000000000);
    cJSON *request = business_request(s);
    s->rx.sequence = s->rx.next_sequence;
    char accepted[33] = {0};
    inject_oom(index);
    esp_err_t e = receive_business(s, request, accepted);
    bool failed = stop_oom();
    assert_complete_business(journal);
    if (failed)
      assert(e == ESP_ERR_NO_MEM && !accepted[0] &&
             !item(journal, "events")->child);
    else
      assert(e == ESP_OK && accepted[0] && item(journal, "events")->child);
    assert(!item(saved, "events")->child);
    cJSON_Delete(request);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u business construction allocation failures leave no partial "
         "event or ledger\n",
         index);
  for (index = 0; index < 1600; ++index) {
    reset();
    session_t *s = authenticate(0);
    cJSON *request = business_request(s);
    char *plain = cJSON_PrintUnformatted(request);
    size_t length = strlen(plain);
    uint8_t encrypted[512], fragment[512];
    encrypted[0] = 0xee;
    memcpy(encrypted + 1, plain, length);
    /* Use one complete encrypted PG message so every failure belongs to the
     * production decrypt/parse/persist/receipt/STORED transaction. */
    s->mtu = 517;
    size_t n =
        gw_pg_fragment(fragment, sizeof(fragment), 1, s->rx.next_sequence,
                       encrypted, length + 1, 0, s->mtu);
    assert(n == length + 13);
    inject_oom(index);
    consume(s, fragment, n, "oom-receipt", now);
    bool failed = stop_oom();
    assert_complete_business(saved);
    if (s->used && s->tx_type == 2) {
      cJSON *ack = parse_json(s->tx + 1, s->tx_length - 1);
      assert(!strcmp(str(ack, "status"), "STORED") &&
             item(saved, "events")->child &&
             !strcmp(str(saved, "lastReceipt"), "oom-receipt"));
      cJSON_Delete(ack);
    }
    cJSON_Delete(request);
    free(plain);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u receive/commit/STORED allocation failures never acknowledge "
         "incomplete business data\n",
         index);
}
static void test_execute_oom(void) {
  for (unsigned disconnect = 0; disconnect < 2; ++disconnect) {
    unsigned index;
    for (index = 0; index < 1600; ++index) {
      reset();
      session_t *s = authenticate(0);
      cJSON *command =
          notify(s, "oom-task", "20112233445566778899aabbccddeeff",
                 disconnect ? "PERIPHERAL_DISCONNECT" : "PERIPHERAL_NOTIFY");
      cJSON *details = NULL;
      require_intent = true;
      physical_calls = 0;
      inject_oom(index);
      esp_err_t e = gw_peripheral_execute(command, now + 100, &details);
      bool failed = stop_oom();
      if (item(saved, "tasks")->child)
        assert_complete_intent();
      else
        assert(physical_calls == 0);
      assert(e != ESP_OK); /* no phone APPLIED/CLOSED proof was supplied */
      if (details) {
        assert(!strcmp(str(details, "radio"), "NRF54") &&
               !strcmp(str(details, "clientId"), "phone") &&
               *str(details, "connectionRef") &&
               !strcmp(str(details, "verification"), "UNKNOWN"));
        cJSON_Delete(details);
      }
      cJSON_Delete(command);
      if (!failed)
        break;
    }
    assert(index < 1600);
    printf("PASS: %u %s allocation failures; every physical action has a "
           "complete persisted intent\n",
           index, disconnect ? "DISCONNECT" : "NOTIFY");
  }
}
static void test_upload_oom(void) {
  unsigned index;
  for (index = 0; index < 1600; ++index) {
    reset();
    session_t *s = authenticate(0);
    cJSON *request = business_request(s);
    s->rx.sequence = s->rx.next_sequence;
    char accepted[33] = {0};
    assert(receive_business(s, request, accepted) == ESP_OK &&
           commit() == ESP_OK);
    cJSON_Delete(request);
    transport_lost = true;
    inject_oom(index);
    esp_err_t e = gw_peripheral_service("tenant", "gateway", NULL, now + 1000);
    bool failed = stop_oom();
    assert_complete_business(saved);
    assert(item(saved, "events")->child);
    if (failed)
      assert(e == ESP_ERR_NO_MEM && http_calls == 0);
    else
      assert(e == ESP_ERR_TIMEOUT && http_calls == 1);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u upload construction allocation failures retain the business "
         "event and make no network request\n",
         index);
}
static void test_ack_and_sequence_oom(void) {
  unsigned index;
  for (index = 0; index < 1600; ++index) {
    reset();
    session_t *s = authenticate(0);
    cJSON *command = notify(s, "ack-oom", "20112233445566778899aabbccddeeff",
                            "PERIPHERAL_NOTIFY"),
          *details = NULL;
    assert(gw_peripheral_execute(command, now + 100, &details) ==
           ESP_ERR_TIMEOUT);
    cJSON_Delete(details);
    cJSON_Delete(command);
    cJSON *task = find_task("ack-oom");
    assert(setstr(task, "state", "EXECUTING"));
    cJSON_SetNumberValue(item(task, "deadlineMs"), (double)now + 1000);
    cJSON *before = cJSON_Duplicate(task, true), *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "schemaVersion", "1.0");
    cJSON_AddNumberToObject(ack, "transportSeq", s->rx.sequence);
    cJSON_AddStringToObject(ack, "inReplyTo",
                            "20112233445566778899aabbccddeeff");
    cJSON_AddStringToObject(ack, "status", "APPLIED");
    cJSON_AddStringToObject(ack, "responseBase64", "AQ==");
    s->captured_ms = now;
    inject_oom(index);
    esp_err_t e = receive_ack(s, ack);
    bool failed = stop_oom();
    task = find_task("ack-oom");
    if (failed)
      assert(e == ESP_ERR_NO_MEM && cJSON_Compare(before, task, true));
    else
      assert(e == ESP_OK && !strcmp(str(task, "state"), "APPLIED") &&
             !strcmp(str(task, "businessStatus"), "APPLIED") &&
             !strcmp(str(task, "responseBase64"), "AQ=="));
    cJSON_Delete(before);
    cJSON_Delete(ack);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u phone APPLIED allocation failures leave task evidence "
         "unchanged\n",
         index);
  /* The previous loop ended with a complete APPLIED task. Result construction
   * must never return successful but incomplete verification evidence. */
  for (index = 0; index < 1600; ++index) {
    cJSON *details = NULL;
    inject_oom(index);
    esp_err_t e = task_result(find_task("ack-oom"), &details);
    bool failed = stop_oom();
    if (failed)
      assert(e == ESP_ERR_NO_MEM && !details);
    else
      assert(e == ESP_OK &&
             !strcmp(str(details, "businessStatus"), "APPLIED") &&
             !strcmp(str(details, "verification"), "VERIFIED") &&
             !strcmp(str(details, "responseBase64"), "AQ=="));
    cJSON_Delete(details);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u task-result allocation failures never return partial "
         "success evidence\n",
         index);
  for (index = 0; index < 1600; ++index) {
    reset();
    session_t *s = authenticate(0);
    uint8_t *bytes = malloc(1);
    *bytes = 0x42;
    assert(queue_tx(s, 0, bytes, 1) == ESP_OK);
    low_next = low_limit = 0;
    uint64_t old = strtoull(str(saved, "lowSequence"), NULL, 10);
    physical_calls = 0;
    inject_oom(index);
    pump_tx(s);
    bool failed = stop_oom();
    uint64_t current = strtoull(str(saved, "lowSequence"), NULL, 10);
    if (failed)
      assert(!physical_calls && current == old);
    else
      assert(physical_calls == 1 && current == old + 64);
    if (!failed)
      break;
  }
  assert(index < 1600);
  printf("PASS: %u UART sequence-reservation allocation failures transmit no "
         "unreserved sequence\n",
         index);
}
int main(void) {
  test_pg_boundaries();
  test_business_handoff();
  test_storage_and_session_loss();
  test_downlink_proof();
  test_inbox_transaction_and_limits();
  test_unauthorized_and_parser();
  test_business_oom();
  test_execute_oom();
  test_upload_oom();
  test_ack_and_sequence_oom();
  puts("PASS: production Peripheral PG framing, independent sessions, durable "
       "STORED/dedup/spool/ACK, and APPLIED-only completion (crypto/device I/O "
       "mocked)");
  return 0;
}
