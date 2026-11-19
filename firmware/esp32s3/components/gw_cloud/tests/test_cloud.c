/* Compile the production state machine, replacing only device/transport I/O. */
#include <time.h>
#ifdef _WIN32
static struct tm *gmtime_r(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) == 0 ? out : NULL;
}
#endif
#include "../gw_cloud.c"
#include <assert.h>

static cJSON *disk, *config_disk, *configured_target;
static bool fail_http, fail_store;
static unsigned actions, requests, resets;
static esp_err_t action_result = ESP_OK, recovery_result = ESP_ERR_TIMEOUT;
static uint64_t random_counter;
static cJSON *transaction;
static esp_app_desc_t running_app = {.version = "0.2.0"};
static unsigned heartbeats;
const esp_app_desc_t *esp_app_get_description(void) { return &running_app; }
const char *esp_err_to_name(esp_err_t e) {
  return e == ESP_OK ? "OK" : "TEST_ERROR";
}
void esp_fill_random(void *p, size_t n) {
  uint8_t *b = p;
  for (size_t i = 0; i < n; i++)
    b[i] = (uint8_t)(++random_counter);
}
void esp_restart(void) { resets++; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t p, TickType_t t) {
  (void)p;
  (void)t;
  return 1;
}
int xSemaphoreGive(SemaphoreHandle_t p) {
  (void)p;
  return 1;
}
int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *p,
                unsigned pri, void *h) {
  (void)f;
  (void)n;
  (void)s;
  (void)p;
  (void)pri;
  (void)h;
  return 1;
}
void vTaskDelay(TickType_t t) { (void)t; }
bool gw_core_time_trusted(void) { return true; }
uint64_t gw_core_mono_ms(void) { return 1000; }
esp_err_t gw_store_put_json(const char *key, const cJSON *v) {
  if (fail_store)
    return ESP_FAIL;
  if (!strcmp(key, JOURNAL_KEY)) {
    cJSON_Delete(disk);
    disk = cJSON_Duplicate(v, true);
  }
  if (!strcmp(key, "cloud/configuration")) {
    cJSON_Delete(config_disk);
    config_disk = cJSON_Duplicate(v, true);
  }
  return ESP_OK;
}
cJSON *gw_store_get_json(const char *key) {
  return !strcmp(key, JOURNAL_KEY) ? cJSON_Duplicate(disk, true)
         : !strcmp(key, "cloud/configuration")
             ? cJSON_Duplicate(config_disk, true)
             : NULL;
}
esp_err_t gw_factory_get_string(const char *key, char *out, size_t n) {
  snprintf(out, n, "%s", !strcmp(key, "namespace") ? "ns_test" : "gw_test");
  return ESP_OK;
}
void gw_core_set_probe_handler(gw_network_probe_fn f, void *p) {
  (void)f;
  (void)p;
}
cJSON *gw_core_state(void) { return cJSON_CreateObject(); }
esp_err_t gw_core_remote_begin(const char *id, const cJSON *n, const char *r,
                               const cJSON *p, uint64_t d, cJSON **o) {
  (void)id;
  (void)n;
  (void)r;
  (void)p;
  (void)d;
  *o = NULL;
  return ESP_ERR_NOT_SUPPORTED;
}
cJSON *gw_core_transaction_by_task(const char *id) {
  (void)id;
  return transaction ? cJSON_Duplicate(transaction, true) : NULL;
}
esp_err_t gw_core_platform_ack(const char *id) {
  (void)id;
  return ESP_OK;
}
esp_err_t gw_core_maintenance_acquire(const char *id) {
  (void)id;
  return ESP_OK;
}
void gw_core_maintenance_release(const char *id) { (void)id; }
esp_err_t gw_core_credential_install(const char *a, const char *b,
                                     const char *c, const char *d,
                                     const char *e, const char *f) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  return ESP_OK;
}
cJSON *gw_core_credential_metadata(void) { return cJSON_CreateArray(); }
bool gw_nrf_identity(uint8_t b[16], uint32_t *c, char v[64]) {
  (void)b;
  (void)c;
  (void)v;
  return false;
}
size_t gw_capture_used_bytes(void) { return 0; }
void gw_capture_upload(const char *n, const char *g, uint64_t d) {
  (void)n;
  (void)g;
  (void)d;
}
esp_err_t gw_actions_execute(const cJSON *c, uint64_t d, cJSON **o) {
  (void)c;
  (void)d;
  actions++;
  *o = cJSON_CreateObject();
  return action_result;
}
esp_err_t gw_actions_configure(const cJSON *p, const cJSON *c, uint64_t r,
                               uint64_t d, cJSON **o) {
  (void)p;
  cJSON_Delete(configured_target);
  configured_target = cJSON_Duplicate(c, true);
  (void)r;
  (void)d;
  *o = NULL;
  return gw_store_put_json("cloud/configuration", c);
}
esp_err_t gw_actions_recover_config(const cJSON *p, const cJSON *c, uint64_t r,
                                    cJSON **o) {
  (void)p;
  (void)c;
  (void)r;
  *o = NULL;
  return recovery_result;
}
esp_err_t gw_actions_recover(const cJSON *c, uint64_t d, cJSON **o) {
  (void)c;
  (void)d;
  *o = NULL;
  return recovery_result;
}
esp_err_t gw_http_json(const char *p, const char *m, const cJSON *b,
                       const char *i, uint64_t d, cJSON **out) {
  (void)m;
  (void)i;
  (void)d;
  requests++;
  *out = NULL;
  if (fail_http)
    return ESP_FAIL;
  if (!strcmp(p, "/api/v1/gateway-ingress/status")) {
    assert(!strcmp(str(b, "firmwareVersion"), running_app.version));
    assert(!strcmp(m, "POST"));
    heartbeats++;
    return ESP_OK;
  }
  assert(!strcmp(p, "/api/v1/gateway-ingress/results"));
  const cJSON *r =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(b, "results"), 0);
  assert(r);
  *out = cJSON_CreateObject();
  cJSON_AddStringToObject(*out, "namespaceId", "ns_test");
  cJSON_AddStringToObject(*out, "gatewayId", "gw_test");
  cJSON_AddStringToObject(*out, "durability", "RESULT_PERSISTED");
  cJSON *items = cJSON_AddArrayToObject(*out, "items"),
        *item = cJSON_CreateObject();
  cJSON_AddItemToArray(items, item);
  cJSON_AddStringToObject(item, "taskId", str(r, "taskId"));
  cJSON_AddStringToObject(item, "resultId", str(r, "resultId"));
  cJSON_AddStringToObject(item, "status", "ACCEPTED");
  return ESP_OK;
}
static cJSON *new_command(const char *id) {
  cJSON *c = cJSON_Parse(
      "{\"schemaVersion\":\"1.0\",\"namespaceId\":\"ns_test\",\"gatewayId\":"
      "\"gw_test\",\"taskId\":\"test\",\"type\":\"DIAGNOSTICS\",\"parameters\":"
      "{\"profile\":\"BASIC\"},\"requestDigest\":"
      "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      "\"createdAt\":\"2026-01-01T00:00:00Z\"}");
  text(c, "taskId", id);
  time_t expiry = time(NULL) + 60;
  struct tm t;
  gmtime_r(&expiry, &t);
  char when[32];
  strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &t);
  text(c, "expiresAt", when);
  return c;
}
int main(void) {
  assert(gw_cloud_init() == ESP_OK);
  heartbeat();
  strcpy(running_app.version, "0.3.1");
  heartbeat();
  assert(heartbeats == 2); /* Local/remote OTA heartbeat uses the running descriptor. */
  cJSON *c = new_command("t1");
  command(c);
  assert(actions == 0 && cJSON_GetArraySize(records()) == 1);
  fail_http = true;
  service_journal();
  assert(actions == 0 && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(
                             find_record("t1"), "pending")) == 1);
  fail_http = false;
  service_journal();
  service_journal();
  assert(actions == 1 && !strcmp(str(find_record("t1"), "phase"), "DONE"));
  command(c);
  service_journal();
  assert(actions == 1);
  text(c, "requestDigest",
       "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  command(c);
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(find_record("t1"),
                                                       "conflictObserved")) &&
         actions == 1);
  cJSON_Delete(c);
  c = new_command("failed-store");
  fail_store = true;
  command(c);
  fail_store = false;
  assert(!find_record("failed-store") && actions == 1);
  cJSON_Delete(c);
  c = new_command("expired");
  text(c, "expiresAt", "2020-01-01T00:00:00Z");
  command(c);
  service_journal();
  assert(actions == 1 &&
         !strcmp(str(find_record("expired"), "state"), "EXPIRED"));
  cJSON_Delete(c);
  c = new_command("ambiguous");
  command(c);
  cJSON *r = find_record("ambiguous");
  text(r, "phase", "INTENT");
  text(r, "executionBoot", boot_id);
  save();
  service_journal();
  assert(actions == 1 &&
         !strcmp(str(find_record("ambiguous"), "state"), "UNKNOWN"));
  cJSON_Delete(c);
  c = new_command("rolled-back-probe");
  command(c);
  r = find_record("rolled-back-probe");
  text(r, "phase", "NETWORK");
  save();
  cJSON *details =
      cJSON_Parse("{\"networkProbe\":{\"interface\":\"WIFI_STA\"}}");
  fail_http = true;
  result_for("rolled-back-probe", "probe_WIFI_STA", "EXECUTING", "NONE",
             details, NULL, "WIFI_STA", 5000);
  fail_http = false;
  cJSON_Delete(details);
  transaction =
      cJSON_Parse("{\"state\":\"ROLLED_BACK\",\"code\":\"TEST_ROLLBACK\"}");
  unsigned before = requests;
  service_journal();
  assert(requests == before);
  assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(
             find_record("rolled-back-probe"), "quarantined")) == 1);
  cJSON_Delete(transaction);
  transaction = NULL;
  cJSON_Delete(c);
  action_result = ESP_ERR_NOT_FINISHED;
  recovery_result = ESP_ERR_NOT_FINISHED;
  c = new_command("ota-trial");
  text(c, "type", "FIRMWARE_UPGRADE");
  command(c);
  service_journal();
  service_journal();
  assert(!cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(find_record("ota-trial"), "phases"),
      "terminal"));
  recovery_result = ESP_OK;
  service_journal();
  assert(!strcmp(str(find_record("ota-trial"), "state"), "SUCCEEDED"));
  cJSON_Delete(c);
  c = new_command("legacy-config");
  text(c, "type", "CONFIGURE");
  put(c, "parameters",
      cJSON_Parse("{\"desiredRevision\":1,\"configuration\":{\"scan\":{"
                  "\"enabled\":true},\"upload\":{\"flushIntervalMs\":500}}}"));
  command(c);
  service_journal();
  assert(!strcmp(str(find_record("legacy-config"), "state"), "SUCCEEDED"));
  cJSON *radio = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(configured_target, "radios"), "nrf54");
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
      cJSON_GetObjectItemCaseSensitive(radio, "scan"), "enabled")));
  assert(num(cJSON_GetObjectItemCaseSensitive(radio, "scan"), "intervalMs") ==
         100);
  assert(!cJSON_GetObjectItemCaseSensitive(configured_target, "scan"));
  assert(num(cJSON_GetObjectItemCaseSensitive(configured_target, "upload"),
             "batchMaxRecords") == 64);
  assert(num(cJSON_GetObjectItemCaseSensitive(configured_target, "upload"),
             "flushIntervalMs") == 500);
  cJSON_Delete(c);
  assert(parse_utc("2024-02-29T00:00:00Z") > 0 &&
         parse_utc("2025-02-29T00:00:00Z") == -1 &&
         parse_utc("2026-11-19T01:00:00+01:00") ==
             parse_utc("2026-11-19T00:00:00Z"));
  cJSON_Delete(journal);
  journal = NULL;
  cJSON_Delete(disk);
  cJSON_Delete(config_disk);
  cJSON_Delete(configured_target);
  puts("cloud: admission, ACK retry, exactly-once intent, expiry, conflict, "
       "probe quarantine passed");
  return 0;
}
