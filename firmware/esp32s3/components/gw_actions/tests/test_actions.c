/* Exercises production journal/config/dispatch code. Mocks replace physical
 * actions and durable storage; digest uses a deterministic test-only hash. */
#include "../gw_actions.c"
#include "monocypher-ed25519.h"
#include <assert.h>
static cJSON *database;
static bool fail_store, lock_busy;
static unsigned local_closed, local_revoked_count;
static unsigned effects, transactions;
static esp_err_t effect_result, recover_result;
static char maintenance[129];
bool gw_capture_upload_valid(const cJSON *v) {
    const cJSON *batch = cJSON_GetObjectItemCaseSensitive(v, "batchMaxRecords"),
                *flush = cJSON_GetObjectItemCaseSensitive(v, "flushIntervalMs");
    return cJSON_IsNumber(batch) && cJSON_IsNumber(flush) && batch->valueint >= 1 &&
           batch->valueint <= 1000 && flush->valueint >= 100 && flush->valueint <= 60000;
}
esp_err_t gw_capture_configure_upload(const cJSON *v) {
    return gw_store_put_json("capture/upload", v);
}
cJSON *gw_capture_upload_configuration(void) {
    cJSON *v = gw_store_get_json("capture/upload");
    return v ? v : cJSON_Parse("{\"batchMaxRecords\":64,\"flushIntervalMs\":250}");
}
cJSON *gw_core_state(void) { return cJSON_CreateObject(); }
esp_err_t gw_core_wifi_scan(bool active, uint64_t deadline, cJSON **aps) {
    (void)active;
    (void)deadline;
    *aps = cJSON_Parse("[{\"ssid\":\"ap\",\"bssid\":\"11:22:33:44:55:66\","
                       "\"channel\":1,\"rssi\":-"
                       "50,\"security\":\"WPA2_PSK\"}]");
    return ESP_OK;
}
uint64_t gw_core_mono_ms(void) { return 100; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)s;
    (void)t;
    return !lock_busy;
}
void xSemaphoreGive(SemaphoreHandle_t s) { (void)s; }
void vTaskDelay(TickType_t t) { (void)t; }
int xTaskCreate(void (*f)(void *), const char *n, unsigned stack, void *p, unsigned priority,
                void *h) {
    (void)f;
    (void)n;
    (void)stack;
    (void)p;
    (void)priority;
    (void)h;
    return pdPASS;
}
esp_err_t gw_core_maintenance_acquire(const char *owner) {
    if (*maintenance && strcmp(maintenance, owner))
        return ESP_ERR_INVALID_STATE;
    snprintf(maintenance, sizeof(maintenance), "%s", owner);
    return ESP_OK;
}
void gw_core_maintenance_release(const char *owner) {
    if (!strcmp(maintenance, owner))
        *maintenance = 0;
}
cJSON *gw_store_get_json(const char *key) {
    return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(database, key), true);
}
esp_err_t gw_store_put_json(const char *key, const cJSON *value) {
    if (fail_store)
        return ESP_FAIL;
    cJSON_DeleteItemFromObjectCaseSensitive(database, key);
    cJSON_AddItemToObject(database, key, cJSON_Duplicate(value, true));
    return ESP_OK;
}
esp_err_t gw_store_transaction(const gw_store_op_t *ops, size_t count) {
    if (fail_store)
        return ESP_FAIL;
    transactions++;
    for (size_t i = 0; i < count; i++) {
        cJSON *j = cJSON_ParseWithLength(ops[i].data, ops[i].size);
        assert(j);
        assert(gw_store_put_json(ops[i].key, j) == ESP_OK);
        cJSON_Delete(j);
    }
    return ESP_OK;
}
/* Crypto interoperability is separately tested against RFC/OpenSSL. */
int mbedtls_sha256(const unsigned char *p, size_t n, unsigned char out[32], int mode) {
    (void)mode;
    uint8_t hash[64];
    crypto_sha512(hash, p, n);
    memcpy(out, hash, 32);
    return 0;
}
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n, const unsigned char *in,
                          size_t length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned value = 0, bits = 0;
    *n = 0;
    if (length % 4)
        return -1;
    for (size_t i = 0; i < length; i++) {
        if (in[i] == '=')
            break;
        const char *p = strchr(alphabet, in[i]);
        if (!p)
            return -1;
        value = (value << 6) | (unsigned)(p - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (*n >= cap)
                return -1;
            out[(*n)++] = (uint8_t)(value >> bits);
        }
    }
    return 0;
}
bool gw_nrf_identity(uint8_t b[16], uint32_t *c, char v[64]) {
    if (b)
        memset(b, 1, 16);
    if (c)
        *c = 0xffff;
    if (v)
        strcpy(v, "test");
    return true;
}
esp_err_t gw_nrf_scan(const cJSON *p, uint64_t r, int64_t d) {
    (void)p;
    (void)r;
    (void)d;
    effects++;
    return effect_result;
}
static uint64_t advertised_revision;
static bool advertising_missing_evidence;
esp_err_t gw_nrf_advertise(const cJSON *p, uint64_t r, int64_t d) {
    advertised_revision=r; return gw_nrf_scan(p,r,d);
}
cJSON *gw_nrf_diagnostics(int64_t d) {
    (void)d;
    cJSON *j=cJSON_CreateObject();
    if (!advertising_missing_evidence && advertised_revision) {
        char revision[24]; snprintf(revision,sizeof(revision),"%llu",(unsigned long long)advertised_revision);
        cJSON_AddStringToObject(j,"advertisingRevision",revision);
        cJSON_AddStringToObject(j,"advertisingState","ACTIVE");
    }
    return j;
}
void gw_capture_radio_enabled(uint8_t r, bool e) {
    (void)r;
    (void)e;
}
esp_err_t gw_ble_init(void) { return ESP_OK; }
esp_err_t gw_ota_init(void) { return ESP_OK; }
void gw_ota_local_tick(void) {}
void gw_ota_local_revoke(void) { local_revoked_count++; }
void gw_ota_local_window_closed(void) { local_closed++; }
void gw_ota_local_boot_ready(void) {}
esp_err_t gw_ota_local_dispatch(const cJSON *r, uint64_t d, cJSON **o) {
    (void)r;
    (void)d;
    *o = cJSON_Parse("{\"code\":\"OK\"}");
    return ESP_OK;
}
esp_err_t gw_gatt_nrf_init(void) { return ESP_OK; }
cJSON *gw_ble_diagnostics(void) { return cJSON_CreateObject(); }
esp_err_t gw_ble_configure(const cJSON *p, bool a, uint64_t r, uint64_t d, cJSON **out) {
    (void)a;
    *out = cJSON_CreateObject();
    return gw_nrf_scan(p, r, (int64_t)d);
}
esp_err_t gw_ble_execute(const cJSON *c, uint64_t d, cJSON **out) {
    (void)c;
    (void)d;
    effects++;
    *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out, "attResponse", true);
    return effect_result;
}
esp_err_t gw_gatt_nrf_execute(const cJSON *c, uint64_t d, cJSON **out) {
    return gw_ble_execute(c, d, out);
}
esp_err_t gw_ota_execute(const cJSON *c, uint64_t d, cJSON **out) {
    return gw_ble_execute(c, d, out);
}
esp_err_t gw_ota_recover(const cJSON *c, uint64_t d, cJSON **out) {
    (void)c;
    (void)d;
    *out = cJSON_CreateObject();
    return recover_result;
}
esp_err_t gw_gatt_nrf_recover(const cJSON *c, uint64_t d, cJSON **out) {
    return gw_ota_recover(c, d, out);
}
static void reset_test(void) {
    cJSON_Delete(database);
    database = cJSON_CreateObject();
    fail_store = false;
    effects = transactions = 0;
    effect_result = recover_result = ESP_OK;
    *maintenance = 0;
}
static cJSON *command(const char *id, const char *type) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "taskId", id);
    cJSON_AddStringToObject(c, "type", type);
    cJSON_AddObjectToObject(c, "parameters");
    return c;
}
int main(void) {
    assert(gw_actions_init() == ESP_OK);
    reset_test();
    cJSON *out = NULL;
    cJSON *local = cJSON_Parse("{\"requestId\":\"local-test\",\"operation\":"
                               "\"OTA_STATUS\",\"body\":{}}");
    ota_ready = false;
    assert(gw_actions_local_ota(local, 10000, &out) == ESP_OK &&
           !strcmp(gw_action_string(out, "code"), "NOT_READY"));
    cJSON_Delete(out);
    ota_ready = true;
    lock_busy = true;
    assert(gw_actions_local_ota(local, 10000, &out) == ESP_OK &&
           !strcmp(gw_action_string(out, "code"), "BUSY"));
    cJSON_Delete(out);
    gw_actions_local_ota_window_closed();
    assert(local_revoked_count == 1 && local_closed == 0 && atomic_load(&local_window_closed));
    lock_busy = false;
    assert(gw_actions_local_ota(local, 10000, &out) == ESP_OK && local_closed == 1 &&
           !atomic_load(&local_window_closed));
    cJSON_Delete(out);
    cJSON_Delete(local);
    out = NULL;
    cJSON *patch = cJSON_Parse("{\"radios\":{\"nrf54\":{\"scan\":{\"enabled\":true}}}}");
    cJSON *complete = cJSON_Parse("{\"radios\":{\"nrf54\":{\"scan\":{\"enabled\":true,"
                                  "\"active\":false,\"intervalMs\":100,"
                                  "\"windowMs\":50},\"filters\":{\"minRssi\":null,"
                                  "\"serviceUuids\":[\"180f\",\"0000180f-0000-"
                                  "1000-8000-00805f9b34fb\"],\"manufacturerIds\":[76,76]},"
                                  "\"beaconRules\":[{\"action\":"
                                  "\"ALLOW\",\"reportKinds\":[\"LEGACY_ADV\"],\"adType\":255,"
                                  "\"offset\":0,\"valueBase64\":"
                                  "\"TAACFQ==\",\"maskBase64\":\"/////w==\"}]}}}");
    struct gw_profile p;
    const cJSON *radio = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(complete, "radios"), "nrf54");
    assert(gw_action_profile(radio, 1, &p) == ESP_OK);
    assert(p.uuid_count == 1 && p.company_count == 1 && p.rule_count == 1 &&
           p.rules[0].length == 4 && p.min_rssi == -128);
    assert(gw_actions_configure(patch, complete, 1, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1 && transactions == 1);
    assert(gw_actions_recover_config(patch, complete, 1, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1);
    assert(gw_actions_configure(patch, complete, 1, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1);
    assert(gw_actions_configure(patch, complete, 2, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 2);
    assert(gw_actions_configure(patch, complete, 1, 10000, &out) == ESP_ERR_INVALID_STATE &&
           effects == 2);
    fail_store = true;
    assert(gw_actions_configure(patch, complete, 3, 10000, &out) != ESP_OK && effects == 2);
    fail_store = false;
    effect_result = ESP_ERR_TIMEOUT;
    assert(gw_actions_configure(patch, complete, 3, 10000, &out) == ESP_ERR_TIMEOUT);
    cJSON_Delete(out);
    assert(effects == 3);
    assert(gw_actions_configure(patch, complete, 4, 10000, &out) == ESP_ERR_NOT_FINISHED &&
           effects == 3);
    cJSON *committed = gw_store_get_json("actions/target/nrf54/scan");
    assert(!strcmp(gw_action_string(committed, "identity"), "2"));
    cJSON_Delete(committed);
    reset_test();
    cJSON *write = command("write-1", "GATT_WRITE");
    assert(gw_actions_execute(write, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1);
    assert(gw_actions_execute(write, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1);
    cJSON_AddStringToObject(write, "changed", "payload");
    assert(gw_actions_execute(write, 10000, &out) == ESP_ERR_INVALID_ARG && effects == 1);
    cJSON_Delete(write);
    reset_test();
    write = command("ambiguous-write", "GATT_WRITE");
    effect_result = ESP_ERR_TIMEOUT;
    assert(gw_actions_execute(write, 10000, &out) == ESP_ERR_TIMEOUT);
    cJSON_Delete(out);
    assert(effects == 1 && *maintenance);
    assert(gw_actions_execute(write, 10000, &out) == ESP_ERR_TIMEOUT);
    cJSON_Delete(out);
    assert(effects == 1);
    cJSON *next = command("next-write", "GATT_WRITE");
    assert(gw_actions_execute(next, 10000, &out) == ESP_ERR_NOT_FINISHED && effects == 1);
    cJSON_Delete(next);
    recover_result = ESP_ERR_TIMEOUT;
    assert(gw_actions_recover(write, 10000, &out) == ESP_ERR_TIMEOUT);
    cJSON_Delete(out);
    assert(effects == 1);
    recover_result = ESP_OK;
    assert(gw_actions_recover(write, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1 && !*maintenance);
    cJSON_Delete(write);
    reset_test();
    cJSON *ota = command("ota", "FIRMWARE_UPGRADE");
    effect_result = ESP_ERR_NOT_FINISHED;
    assert(gw_actions_execute(ota, 10000, &out) == ESP_ERR_NOT_FINISHED);
    cJSON_Delete(out);
    recover_result = ESP_ERR_NOT_FINISHED;
    assert(gw_actions_recover(ota, 10000, &out) == ESP_ERR_NOT_FINISHED);
    cJSON_Delete(out);
    assert(effects == 1);
    recover_result = ESP_OK;
    assert(gw_actions_recover(ota, 10000, &out) == ESP_OK);
    cJSON_Delete(out);
    assert(effects == 1);
    cJSON_Delete(ota);
    reset_test();
    cJSON *upload_patch = cJSON_Parse("{\"upload\":{\"batchMaxRecords\":100}}"), *up = NULL;
    cJSON_AddItemToObject(complete, "upload",
                          cJSON_Parse("{\"batchMaxRecords\":100,\"flushIntervalMs\":250}"));
    assert(gw_actions_configure(upload_patch, complete, 10, 10000, &out) == ESP_OK && effects == 0);
    cJSON_Delete(out);
    up = gw_capture_upload_configuration();
    assert(cJSON_GetObjectItemCaseSensitive(up, "batchMaxRecords")->valueint == 100 &&
           cJSON_GetObjectItemCaseSensitive(up, "flushIntervalMs")->valueint == 250);
    cJSON_Delete(up);
    cJSON_Delete(upload_patch);
    upload_patch =
        cJSON_Parse("{\"scan\":{\"enabled\":true},\"upload\":{\"flushIntervalMs\":500}}");
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(complete, "upload"),
                                           "flushIntervalMs", cJSON_CreateNumber(500));
    assert(gw_actions_configure(upload_patch, complete, 11, 10000, &out) == ESP_OK && effects == 1);
    cJSON_Delete(out);
    cJSON_Delete(upload_patch);
    cJSON *diagnostic = command("wifi-diagnostics", "DIAGNOSTICS"),
          *params = cJSON_GetObjectItemCaseSensitive(diagnostic, "parameters");
    cJSON_AddStringToObject(params, "profile", "NETWORK");
    cJSON_AddStringToObject(params, "wifiScan", "PASSIVE");
    assert(gw_actions_execute(diagnostic, 10000, &out) == ESP_OK);
    cJSON *ap = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(out, "accessPoints"), 0);
    assert(cJSON_GetObjectItemCaseSensitive(ap, "rssiDbm")->valueint == -50 &&
           !strcmp(gw_action_string(ap, "authentication"), "WPA2_PSK"));
    cJSON_Delete(out);
    cJSON_Delete(diagnostic);
    reset_test();
    cJSON *adv_patch=cJSON_Parse("{\"radios\":{\"nrf54\":{\"advertising\":{\"enabled\":true}}}}"),
          *adv_complete=cJSON_Duplicate(complete,true);
    cJSON *adv_radio=cJSON_GetObjectItem(cJSON_GetObjectItem(adv_complete,"radios"),"nrf54");
    cJSON_AddItemToObject(adv_radio,"advertising",cJSON_Parse("{\"enabled\":true,\"mode\":\"NONCONNECTABLE\"}"));
    assert(gw_actions_configure(adv_patch,adv_complete,1,10000,&out)==ESP_ERR_INVALID_STATE && effects==0);
    cJSON_ReplaceItemInObject(cJSON_GetObjectItem(adv_radio,"scan"),"enabled",cJSON_CreateFalse());
    assert(gw_actions_configure(adv_patch,adv_complete,1,10000,&out)==ESP_OK && effects==1);
    assert(!strcmp(gw_action_string(out,"advertisingState"),"ACTIVE"));cJSON_Delete(out);
    reset_test(); advertising_missing_evidence=true;
    assert(gw_actions_configure(adv_patch,adv_complete,2,10000,&out)==ESP_ERR_TIMEOUT && effects==1);
    cJSON_Delete(out);assert(!gw_store_get_json("actions/target/nrf54/advertising"));
    cJSON_Delete(adv_patch);cJSON_Delete(adv_complete);
    cJSON_Delete(patch);
    cJSON_Delete(complete);
    cJSON_Delete(database);
    puts("PASS: actions intent/replay/conflict/storage failure/UNKNOWN/OTA "
         "recovery and profile "
         "conversion");
    return 0;
}
