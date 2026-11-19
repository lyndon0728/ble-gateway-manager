/* Production adapter + real cJSON/Mbed TLS, only hardware and HTTP replaced. */
#include <sys/time.h>
#include <time.h>
static int capture_gettimeofday(struct timeval *, void *);
#define gettimeofday capture_gettimeofday
#ifdef _WIN32
static struct tm *gmtime_r(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) == 0 ? out : NULL;
}
#endif
#include "../gw_capture.c"
#include <assert.h>

static uint8_t flash[GW_SPOOL_BYTES], peer_boot[16] = {9};
static uint64_t now_ms = 1000, random_counter;
static int capture_gettimeofday(struct timeval *out, void *zone) {
  (void)zone;
  out->tv_sec = (time_t)(1700000000 + now_ms / 1000);
  out->tv_usec = (long)(now_ms % 1000) * 1000;
  return 0;
}
static unsigned http_mode, http_calls;
static cJSON *last_request, *examples;
static cJSON *saved_upload, *saved_configuration;
static bool fail_store;
esp_err_t gw_store_put_json(const char *key, const cJSON *value) {
  assert(!strcmp(key, "capture/upload"));
  if (fail_store)
    return ESP_FAIL;
  cJSON_Delete(saved_upload);
  saved_upload = cJSON_Duplicate(value, true);
  return ESP_OK;
}
cJSON *gw_store_get_json(const char *key) {
  return cJSON_Duplicate(!strcmp(key, "capture/upload") ? saved_upload
                                                        : saved_configuration,
                         true);
}
esp_err_t gw_store_get(const char *key, void *out, size_t *size) {
  (void)key;
  (void)out;
  (void)size;
  return saved_upload ? ESP_OK : ESP_ERR_NOT_FOUND;
}
static const esp_partition_t fake_partition = {GW_SPOOL_BYTES, true};
bool esp_flash_encryption_enabled(void) { return true; }
bool gw_core_time_trusted(void) { return true; }
int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
void esp_fill_random(void *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    ((uint8_t *)p)[i] = (uint8_t)++random_counter;
}
uint32_t esp_random(void) { return (uint32_t)++random_counter; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
  (void)s;
  (void)t;
  return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s) {
  (void)s;
  return 1;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *ctx,
                unsigned priority, void *handle) {
  (void)fn;
  (void)name;
  (void)stack;
  (void)ctx;
  (void)priority;
  (void)handle;
  return 1;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; }
bool gw_nrf_identity(uint8_t *boot, uint32_t *caps, char *version) {
  if (boot)
    memcpy(boot, peer_boot, 16);
  if (caps)
    *caps = 0;
  if (version)
    strcpy(version, "test");
  return true;
}
esp_err_t gw_nrf_command(uint8_t op, const void *args, size_t n,
                         uint8_t *status, void *out, size_t *size,
                         int64_t deadline) {
  (void)op;
  (void)args;
  (void)n;
  (void)status;
  (void)out;
  (void)size;
  (void)deadline;
  return ESP_ERR_TIMEOUT;
}
const esp_partition_t *esp_partition_find_first(int type, int subtype,
                                                const char *name) {
  assert(type == 1 && subtype == 0x40 && !strcmp(name, "gwspool"));
  return &fake_partition;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t at, void *out,
                             size_t n) {
  (void)p;
  assert(at + n <= sizeof(flash));
  memcpy(out, flash + at, n);
  return ESP_OK;
}
esp_err_t esp_partition_read_raw(const esp_partition_t *p, size_t at, void *out,
                                 size_t n) {
  return esp_partition_read(p, at, out, n);
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t at,
                              const void *data, size_t n) {
  (void)p;
  assert(!(at % 16) && !(n % 16) && at + n <= sizeof(flash));
  for (size_t i = 0; i < n; i++) {
    uint8_t v = ((const uint8_t *)data)[i];
    assert((flash[at + i] & v) == v);
    flash[at + i] = v;
  }
  return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t at,
                                    size_t n) {
  (void)p;
  assert(!(at % 4096) && !(n % 4096));
  memset(flash + at, 255, n);
  return ESP_OK;
}
esp_err_t gw_http_json(const char *path, const char *method, const cJSON *body,
                       const char *interface, uint64_t deadline, cJSON **out) {
  (void)interface;
  assert(!strcmp(path, "/api/v1/gateway-ingress/batches") &&
         !strcmp(method, "POST") && deadline > now_ms);
  http_calls++;
  cJSON_Delete(last_request);
  last_request = cJSON_Duplicate(body, true);
  *out = NULL;
  if (http_mode == 7)
    return ESP_ERR_TIMEOUT;
  *out = cJSON_CreateObject();
  cJSON_AddStringToObject(*out, "namespaceId", "ns_test");
  cJSON_AddStringToObject(*out, "gatewayId",
                          http_mode == 1 ? "wrong" : "gw_test");
  cJSON_AddStringToObject(*out, "batchId", str(body, "batchId"));
  cJSON_AddStringToObject(*out, "schemaVersion", "1.1");
  cJSON_AddStringToObject(*out, "receiverId", "receiver");
  cJSON_AddStringToObject(*out, "traceId", "trace");
  cJSON_AddStringToObject(*out, "durability", "RAW_PERSISTED");
  cJSON *items = cJSON_AddArrayToObject(*out, "items");
  unsigned index = 0;
  if (http_mode != 2)
    for (const cJSON *event =
             cJSON_GetObjectItemCaseSensitive(body, "events")->child;
         event; event = event->next, index++) {
      if (http_mode == 8 && index >= 2)
        continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "eventId", str(event, "eventId"));
      cJSON_AddStringToObject(item, "status",
                              http_mode == 4 ? "REJECTED_RETRYABLE"
                              : http_mode == 5 || (http_mode == 8 && index == 1)
                                  ? "REJECTED_PERMANENT"
                                  : "ACCEPTED");
      if (http_mode == 4)
        cJSON_AddNumberToObject(item, "retryAfterMs", 1200);
      cJSON_AddItemToArray(items, item);
      if (http_mode == 3)
        cJSON_AddItemToArray(items, cJSON_Duplicate(item, true));
    }
  return ESP_OK;
}
static void adv(uint8_t p[27], uint64_t captured) {
  memset(p, 0, 27);
  gw_put_u64(p, captured);
  p[9] = 1;
  p[10] = 0;
  memset(p + 11, 0xaa, 6);
  p[17] = (uint8_t)-60;
  p[18] = 128;
  p[19] = 1;
  p[21] = 255;
  gw_put_u16(p + 22, 3);
  p[24] = 2;
  p[25] = 1;
  p[26] = 6;
}
static unsigned states(unsigned state) {
  unsigned n = 0;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) == state)
      n++;
  return n;
}
static void save_example(void) {
  cJSON_AddItemToArray(examples, cJSON_Duplicate(last_request, true));
}
static void upload(unsigned mode) {
  http_mode = mode;
  next_upload = 0;
  now_ms += upload_flush_ms;
  (void)gw_capture_upload("ns_test", "gw_test", now_ms + 10000);
}
int main(int argc, char **argv) {
  memset(flash, 255, sizeof(flash));
  assert(gw_capture_init() == ESP_OK);
  examples = cJSON_CreateArray();
  clock_sample(1000);
  assert(gw_capture_time_anchor(peer_boot, 1000, 1004, 1001, 1002, false) ==
         ESP_OK);
  assert(gw_capture_time_anchor(peer_boot, 1000, 1051, 1001, 1001, false) ==
         ESP_ERR_INVALID_ARG);
  assert(gw_capture_time_anchor(peer_boot, 1000, 1004, 1001, 1002, true) ==
         ESP_ERR_INVALID_ARG);
  gw_capture_radio_enabled(0, true);
  gw_capture_radio_enabled(1, true);
  struct gw_frame f = {.type = 0x20, .msg_id = 1, .payload_len = 37};
  gw_put_u16(f.payload, 1);
  gw_put_u64(f.payload + 2, 1);
  adv(f.payload + 10, 1100);
  now_ms = 1105;
  assert(gw_capture_nrf(peer_boot, &f, NULL) == 0);
  assert(states(GW_SP_PENDING) == 1);
  assert(gw_capture_nrf(peer_boot, &f, NULL) == 0);
  assert(states(GW_SP_PENDING) == 1);
  uint8_t local[27];
  adv(local, 1101);
  now_ms = 1110;
  assert(gw_capture_esp(local, 27) == ESP_OK);
  assert(states(GW_SP_PENDING) == 2);
  now_ms = 1300;
  assert(finalize(false) == GW_SP_OK);
  assert(states(GW_SP_READY) == 1 && merged_count == 1);
  upload(1);
  assert(states(GW_SP_READY) == 1);
  const cJSON *event = cJSON_GetArrayItem(
                  cJSON_GetObjectItemCaseSensitive(last_request, "events"), 0),
              *capture = cJSON_GetObjectItemCaseSensitive(event, "capture");
  char stable_id[80];
  strcpy(stable_id, str(event, "eventId"));
  assert(!strcmp(str(capture, "radio"), "NRF54") &&
         cJSON_GetArraySize(
             cJSON_GetObjectItemCaseSensitive(capture, "observedBy")) == 2);
  save_example();
  assert(gw_capture_adv_evidence(stable_id, "AAAAAAAAAAAA", "PUBLIC", peer_boot,
                                 5000));
  upload(2);
  assert(states(GW_SP_READY) == 1);
  upload(3);
  assert(states(GW_SP_READY) == 1);
  upload(4);
  assert(states(GW_SP_READY) == 1);
  unsigned before = http_calls;
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) ==
             ESP_ERR_TIMEOUT &&
         http_calls == before);
  upload(7);
  assert(states(GW_SP_READY) == 1);
  event = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(last_request, "events"), 0);
  assert(!strcmp(stable_id, str(event, "eventId")));
  upload(0);
  assert(states(GW_SP_READY) == 0);
  assert(gw_capture_nrf(peer_boot, &f, NULL) == 0 &&
         states(GW_SP_PENDING) == 0);
  assert(gw_capture_adv_evidence(stable_id, "AAAAAAAAAAAA", "PUBLIC", peer_boot,
                                 5000));
  gw_capture_radio_enabled(0, false);
  gw_capture_radio_enabled(1, true);
  local[8] = 1;
  local[9] = 0;
  assert(gw_capture_esp(local, 27) == ESP_OK);
  upload(5);
  assert(states(GW_SP_QUARANTINE) == 1);
  save_example();
  uint8_t gatt[26] = {0};
  gw_put_u64(gatt, now_ms);
  gw_put_u32(gatt + 8, 3);
  gatt[12] = 1;
  memset(gatt + 13, 0xbb, 6);
  gw_put_u16(gatt + 19, 42);
  gw_put_u16(gatt + 22, 2);
  gatt[24] = 1;
  gatt[25] = 2;
  assert(gw_capture_esp_gatt(esp_boot, gatt, sizeof(gatt)) == ESP_OK);
  upload(0);
  save_example();
  f.type = 0x22;
  f.msg_id = 2;
  f.payload_len = 35;
  memset(f.payload, 0, sizeof(f.payload));
  gw_put_u16(f.payload, 1);
  gw_put_u64(f.payload + 2, 2);
  gw_put_u32(f.payload + 18, 8);
  gw_put_u16(f.payload + 29, 1);
  gw_put_u16(f.payload + 32, 1);
  f.payload[34] = 42;
  assert(gw_capture_nrf(peer_boot, &f, NULL) == 0);
  assert(states(GW_SP_INBOX) == 1);
  cJSON *inbox = gw_capture_peripheral_peek();
  assert(inbox && !strcmp(str(inbox, "trust"), "UNAUTHENTICATED"));
  before = http_calls;
  upload(0);
  assert(http_calls == before);
  assert(gw_capture_peripheral_ack("invalid") == ESP_ERR_NOT_FOUND);
  assert(gw_capture_peripheral_ack(str(inbox, "receiptToken")) == ESP_OK);
  cJSON_Delete(inbox);
  assert(states(GW_SP_INBOX) == 0);
  uint8_t address[6] = {0};
  assert(gw_capture_link_down(0, peer_boot, 2, 0, address, "DISCONNECTED") ==
         ESP_OK);
  upload(0);
  save_example();
  char *original = cJSON_PrintUnformatted(cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(last_request, "events"), 0));
  now_ms += 10;
  assert(gw_capture_link_down(0, peer_boot, 2, 0, address, "DISCONNECTED") ==
         ESP_OK);
  upload(0);
  char *repeat = cJSON_PrintUnformatted(cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(last_request, "events"), 0));
  assert(!strcmp(original, repeat));
  free(original);
  free(repeat);
  for (unsigned i = 0; i < 3; i++)
    assert(gw_capture_esp(local, 27) == ESP_OK);
  assert(!finalize(false));
  assert(states(GW_SP_READY) == 3);
  upload(8);
  assert(states(GW_SP_READY) == 1 && states(GW_SP_QUARANTINE) == 2);
  save_example();
  upload(0); /* Release the missing ACK item from the preceding mixed batch. */
  cJSON *cfg = cJSON_Parse("{\"batchMaxRecords\":0,\"flushIntervalMs\":250}");
  assert(!gw_capture_upload_valid(cfg));
  assert(gw_capture_configure_upload(cfg) == ESP_ERR_INVALID_ARG);
  cJSON_Delete(cfg);
  cfg = cJSON_Parse(
      "{\"batchMaxRecords\":2,\"batchMaxRecords\":3,\"flushIntervalMs\":250}");
  assert(!gw_capture_upload_valid(cfg));
  cJSON_Delete(cfg);
  cfg = cJSON_Parse("{\"batchMaxRecords\":2,\"flushIntervalMs\":60000}");
  assert(gw_capture_upload_valid(cfg));
  fail_store = true;
  assert(gw_capture_configure_upload(cfg) == ESP_FAIL);
  assert(upload_batch_max == 64 && upload_flush_ms == 250);
  fail_store = false;
  assert(gw_capture_configure_upload(cfg) == ESP_OK);
  cJSON_Delete(cfg);
  before = http_calls;
  adv(local, now_ms);
  assert(gw_capture_esp(local, 27) == ESP_OK);
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before);
  assert(gw_capture_esp(local, 27) == ESP_OK);
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before + 1);
  assert(cJSON_GetArraySize(
             cJSON_GetObjectItemCaseSensitive(last_request, "events")) == 2);
  cfg = cJSON_Parse("{\"batchMaxRecords\":1000,\"flushIntervalMs\":100}");
  assert(gw_capture_configure_upload(cfg) == ESP_OK);
  cJSON_Delete(cfg);
  for (unsigned i = 0; i < 210; i++)
    assert(gw_capture_esp(local, 27) == ESP_OK);
  before = http_calls;
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before);
  now_ms += 99;
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before);
  now_ms++;
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before + 1);
  assert(cJSON_GetArraySize(
             cJSON_GetObjectItemCaseSensitive(last_request, "events")) == 200);
  assert(states(GW_SP_READY) == 10);
  save_example();
  assert(gw_capture_upload("ns_test", "gw_test", now_ms + 10000) == ESP_OK &&
         http_calls == before + 2);
  assert(cJSON_GetArraySize(
             cJSON_GetObjectItemCaseSensitive(last_request, "events")) == 10);
  assert(states(GW_SP_READY) == 0);
  save_example();
  upload_batch_max = 64;
  upload_flush_ms = 250;
  assert(restore_upload() == ESP_OK && upload_batch_max == 1000 &&
         upload_flush_ms == 100);
  cJSON *readback = gw_capture_upload_configuration();
  assert(gw_capture_upload_valid(readback));
  cJSON_Delete(readback);
  cJSON_Delete(saved_upload);
  saved_upload = NULL;
  saved_configuration = cJSON_Parse("{\"upload\":{\"flushIntervalMs\":3333}}");
  assert(restore_upload() == ESP_OK && upload_batch_max == 64 &&
         upload_flush_ms == 3333);
  cJSON_Delete(saved_configuration);
  saved_configuration = NULL;
  if (argc > 1) {
    FILE *out = fopen(argv[1], "wb");
    assert(out);
    char *json = cJSON_PrintUnformatted(examples);
    assert(fwrite(json, 1, strlen(json), out) == strlen(json));
    fclose(out);
    free(json);
  }
  cJSON_Delete(examples);
  cJSON_Delete(last_request);
  gw_spool_close(&spool);
  puts("capture adapter: production pair/finalization, immutable retry, "
       "per-item ACK, backoff, quarantine, safe inbox, idempotent link event "
       "passed");
  return 0;
}
