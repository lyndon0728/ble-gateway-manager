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
static bool wrong_result_ack;
static const char *result_ack_status = "ACCEPTED";
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
bool gw_core_time_bounds(uint64_t *lower,uint64_t *upper) {
  if(lower)*lower=(uint64_t)time(NULL)*1000;
  if(upper)*upper=(uint64_t)time(NULL)*1000;
  return true;
}
uint64_t gw_core_time_deadline(uint64_t expires) {
  uint64_t now=(uint64_t)time(NULL)*1000;
  return 1000+(expires>now?expires-now:0);
}
esp_err_t gw_signed_time_refresh(const char *boot) { (void)boot; return ESP_OK; }
esp_err_t gw_transport_init(const char *ns,const char *gw) { (void)ns;(void)gw;return ESP_OK; }
void gw_transport_service(void) {}
void gw_capture_esp_boot(uint8_t boot[16]) {memset(boot,1,16);}
void gw_cloud_sync(const char *ns,const char *gw,const char *boot){(void)ns;(void)gw;(void)boot;}
esp_err_t gw_cloud_recovery_check(const cJSON *c,const char *ns,const char *gw,uint64_t d){(void)c;(void)ns;(void)gw;(void)d;return ESP_OK;}
esp_err_t gw_peripheral_init(void){return ESP_OK;}
esp_err_t gw_peripheral_service(const char *ns,const char *gw,const char *i,uint64_t d){(void)ns;(void)gw;(void)i;(void)d;return ESP_OK;}
esp_err_t gw_peripheral_execute(const cJSON *c,uint64_t d,cJSON **out){(void)c;(void)d;*out=NULL;return ESP_ERR_NOT_SUPPORTED;}
esp_err_t gw_peripheral_recover(const cJSON *c,uint64_t d,cJSON **out){(void)c;(void)d;*out=NULL;return ESP_ERR_TIMEOUT;}
esp_err_t gw_transport_json(const char *path,const char *method,const cJSON *body,
 const char *interface,uint64_t deadline,cJSON **out) {
 return gw_http_json(path,method,body,interface,deadline,out);
}
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
  cJSON_AddStringToObject(item, "resultId",
                          wrong_result_ack ? "unrelated-result" : str(r, "resultId"));
  cJSON_AddStringToObject(item, "status", result_ack_status);
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
static cJSON *revision_command(const char *id, unsigned revision) {
  cJSON *c = new_command(id);
  text(c, "type", "CONFIGURE");
  cJSON *params = cJSON_GetObjectItemCaseSensitive(c, "parameters");
  put(params, "desiredRevision", cJSON_CreateNumber(revision));
  put(params, "configuration", cJSON_Parse("{\"upload\":{\"flushIntervalMs\":500}}"));
  command(c);
  return c;
}
static void revision_ack_tests(void) {
  /* Model revision 1 persisted remotely but its response lost; the server may
   * now issue revision 2, whose result ACK arrives before the retry of 1. */
  put(journal, "reportedRevision", cJSON_CreateNumber(0));
  assert(save() == ESP_OK);
  cJSON *older = revision_command("revision-old", 1);
  fail_http = true;
  finish_config(older, NULL, 5000);
  fail_http = false;
  assert(num(journal, "reportedRevision") == 0);
  cJSON *pending_old = cJSON_Duplicate(cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(find_record("revision-old"), "pending"), 0), true);
  assert(pending_old);
  cJSON *newer = revision_command("revision-new", 2);
  finish_config(newer, NULL, 5000);
  assert(num(journal, "reportedRevision") == 2);
  result_ack_status = "DUPLICATE";
  assert(deliver(pending_old, NULL, 5000) == ESP_OK);
  assert(num(journal, "reportedRevision") == 2 && num(disk, "reportedRevision") == 2);
  /* Replaying a completed finish path must not reintroduce its old revision. */
  finish_config(older, NULL, 5000);
  assert(num(journal, "reportedRevision") == 2);
  cJSON_Delete(journal);
  journal = NULL;
  assert(gw_cloud_init() == ESP_OK);
  assert(num(journal, "reportedRevision") == 2);
  assert(deliver(pending_old, NULL, 5000) == ESP_OK);
  assert(num(journal, "reportedRevision") == 2);
  cJSON_Delete(pending_old);
  cJSON_Delete(older);
  cJSON_Delete(newer);

  cJSON *next = revision_command("revision-next", 3);
  wrong_result_ack = true;
  finish_config(next, NULL, 5000);
  assert(num(journal, "reportedRevision") == 2 && num(disk, "reportedRevision") == 2);
  wrong_result_ack = false;
  result_ack_status = "REJECTED_PERMANENT";
  finish_config(next, NULL, 5000);
  assert(num(journal, "reportedRevision") == 2);
  result_ack_status = "ACCEPTED";
  finish_config(next, NULL, 5000);
  assert(num(journal, "reportedRevision") == 3);
  cJSON_Delete(next);

  /* Numeric strings, fractions, negatives and unsafe numbers cannot become
   * a reporting revision even with an otherwise matching result ACK. */
  const char *bad[] = {"\"9\"", "3.5", "-1", "9007199254740992", "null"};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
    char id[32];
    snprintf(id, sizeof(id), "revision-invalid-%u", i);
    cJSON *c = revision_command(id, 4), *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "reportedConfigurationRevision", cJSON_Parse(bad[i]));
    assert(result_for(id, "terminal", "SUCCEEDED", "VERIFIED", d, NULL, NULL, 5000) == ESP_OK);
    assert(num(journal, "reportedRevision") == 3 && num(disk, "reportedRevision") == 3);
    cJSON_Delete(d);
    cJSON_Delete(c);
  }
  cJSON *failed = revision_command("revision-failed", 4),
        *d = cJSON_Parse("{\"reportedConfigurationRevision\":4}");
  assert(result_for("revision-failed", "terminal", "FAILED", "NONE", d,
                    "TEST_FAILURE", NULL, 5000) == ESP_OK);
  assert(num(journal, "reportedRevision") == 3);
  cJSON_Delete(failed);
  cJSON_Delete(d);
  cJSON *limit = revision_command("revision-limit", 4);
  d = cJSON_Parse("{\"reportedConfigurationRevision\":9007199254740991}");
  assert(result_for("revision-limit", "terminal", "SUCCEEDED", "VERIFIED", d,
                    NULL, NULL, 5000) == ESP_OK);
  assert(num(journal, "reportedRevision") == 9007199254740991.0 &&
         num(disk, "reportedRevision") == 9007199254740991.0);
  cJSON_Delete(limit);
  cJSON_Delete(d);
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
  revision_ack_tests();
  cJSON_Delete(journal);
  journal = NULL;
  cJSON_Delete(disk);
  cJSON_Delete(config_disk);
  cJSON_Delete(configured_target);
  puts("cloud: admission, ACK retry, exactly-once intent, expiry, conflict, "
       "probe quarantine, monotonic configuration ACK/restart passed");
  return 0;
}
