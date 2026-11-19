#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_actions_internal.h"
#include "gw_capture.h"
#include "gw_nrf.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SemaphoreHandle_t action_lock;
const char *gw_action_string(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static int nibble(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
bool gw_action_hex(const char *s, uint8_t *out, size_t n) {
    if (!s || strlen(s) != 2 * n)
        return false;
    for (size_t i = 0; i < n; i++) {
        int a = nibble(s[2 * i]), b = nibble(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (uint8_t)(16 * a + b);
    }
    return true;
}
void gw_action_hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 15];
    }
    out[2 * n] = 0;
}
bool gw_action_uuid(const char *s, uint8_t out[16]) {
    if (!s)
        return false;
    size_t n = strlen(s);
    char text[37], compact[33];
    if (n == 4) {
        snprintf(text, sizeof(text), "0000%s-0000-1000-8000-00805f9b34fb", s);
        return gw_action_uuid(text, out);
    }
    if (n == 8) {
        snprintf(text, sizeof(text), "%s-0000-1000-8000-00805f9b34fb", s);
        return gw_action_uuid(text, out);
    }
    if (n != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
        return false;
    size_t j = 0;
    for (size_t i = 0; i < 36; i++)
        if (s[i] != '-') {
            if (j >= 32)
                return false;
            compact[j++] = s[i];
        }
    compact[j] = 0;
    return j == 32 && gw_action_hex(compact, out, 16);
}
static bool integer(const cJSON *o, const char *key, int min, int max, int fallback, int *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        *out = fallback;
        return true;
    }
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        floor(v->valuedouble) != v->valuedouble || v->valuedouble < min || v->valuedouble > max)
        return false;
    *out = (int)v->valuedouble;
    return true;
}
static int uuid_cmp(const void *a, const void *b) { return memcmp(a, b, 16); }
static int company_cmp(const void *a, const void *b) {
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}
esp_err_t gw_action_profile(const cJSON *o, uint64_t revision, struct gw_profile *p) {
    const cJSON *scan = cJSON_GetObjectItemCaseSensitive(o, "scan"),
                *filters = cJSON_GetObjectItemCaseSensitive(o, "filters"), *v;
    if (!p || !revision || !cJSON_IsObject(scan) || !cJSON_IsObject(filters))
        return ESP_ERR_INVALID_ARG;
    gw_profile_default(p);
    p->revision = revision;
    int interval, window, rssi = -128;
    if (!integer(scan, "intervalMs", 10, 10240, 100, &interval) ||
        !integer(scan, "windowMs", 10, 10240, 50, &window) || window > interval)
        return ESP_ERR_INVALID_ARG;
    v = cJSON_GetObjectItemCaseSensitive(filters, "minRssi");
    if (v && !cJSON_IsNull(v) && !integer(filters, "minRssi", -127, 20, -128, &rssi))
        return ESP_ERR_INVALID_ARG;
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(scan, "enabled"),
                *active = cJSON_GetObjectItemCaseSensitive(scan, "active");
    if (!cJSON_IsBool(enabled) || !cJSON_IsBool(active))
        return ESP_ERR_INVALID_ARG;
    p->scan_flags = (cJSON_IsTrue(enabled) ? 1 : 0) | (cJSON_IsTrue(active) ? 2 : 0);
    p->min_rssi = (int8_t)rssi;
    p->interval_ticks = (uint16_t)((interval * 8 + 2) / 5);
    p->window_ticks = (uint16_t)((window * 8 + 2) / 5);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(filters, "serviceUuids");
    if (list && !cJSON_IsArray(list))
        return ESP_ERR_INVALID_ARG;
    cJSON_ArrayForEach(v, list) {
        if (p->uuid_count >= GW_UUIDS_MAX || !cJSON_IsString(v) ||
            !gw_action_uuid(v->valuestring, p->uuids[p->uuid_count]))
            return ESP_ERR_INVALID_ARG;
        p->uuid_count++;
    }
    qsort(p->uuids, p->uuid_count, 16, uuid_cmp);
    unsigned used = 0;
    for (unsigned i = 0; i < p->uuid_count; i++)
        if (!used || memcmp(p->uuids[i], p->uuids[used - 1], 16))
            memcpy(p->uuids[used++], p->uuids[i], 16);
    p->uuid_count = (uint8_t)used;
    list = cJSON_GetObjectItemCaseSensitive(filters, "manufacturerIds");
    if (list && !cJSON_IsArray(list))
        return ESP_ERR_INVALID_ARG;
    cJSON_ArrayForEach(v, list) {
        if (p->company_count >= GW_UUIDS_MAX || !cJSON_IsNumber(v) || v->valuedouble < 0 ||
            v->valuedouble > 65535 || floor(v->valuedouble) != v->valuedouble)
            return ESP_ERR_INVALID_ARG;
        p->companies[p->company_count++] = (uint16_t)v->valuedouble;
    }
    qsort(p->companies, p->company_count, 2, company_cmp);
    used = 0;
    for (unsigned i = 0; i < p->company_count; i++)
        if (!used || p->companies[i] != p->companies[used - 1])
            p->companies[used++] = p->companies[i];
    p->company_count = (uint8_t)used;
    list = cJSON_GetObjectItemCaseSensitive(o, "beaconRules");
    if (!list)
        list = cJSON_GetObjectItemCaseSensitive(filters, "beaconRules");
    if (list && !cJSON_IsArray(list))
        return ESP_ERR_INVALID_ARG;
    cJSON_ArrayForEach(v, list) {
        if (p->rule_count == GW_RULES_MAX || !cJSON_IsObject(v))
            return ESP_ERR_INVALID_ARG;
        struct gw_beacon_rule *r = &p->rules[p->rule_count];
        const char *action = gw_action_string(v, "action");
        if (strcmp(action, "ALLOW") && strcmp(action, "DENY"))
            return ESP_ERR_INVALID_ARG;
        r->action = !strcmp(action, "ALLOW");
        const cJSON *k, *kinds = cJSON_GetObjectItemCaseSensitive(v, "reportKinds");
        if (!cJSON_IsArray(kinds) || !cJSON_GetArraySize(kinds))
            return ESP_ERR_INVALID_ARG;
        cJSON_ArrayForEach(k, kinds) {
            const char *names[] = {"LEGACY_ADV", "LEGACY_SCAN_RSP", "EXT_ADV", "EXT_SCAN_RSP"};
            bool found = false;
            for (unsigned i = 0; i < 4; i++)
                if (cJSON_IsString(k) && !strcmp(k->valuestring, names[i])) {
                    r->kind_mask |= (uint8_t)(1u << i);
                    found = true;
                }
            if (!found)
                return ESP_ERR_INVALID_ARG;
        }
        int ad_type, offset;
        if (!integer(v, "adType", 1, 255, 0, &ad_type) ||
            !integer(v, "offset", 0, 65535, 0, &offset) || !ad_type)
            return ESP_ERR_INVALID_ARG;
        r->ad_type = (uint8_t)ad_type;
        r->offset = (uint16_t)offset;
        size_t a = 0, b = 0;
        const char *value = gw_action_string(v, "valueBase64"),
                   *mask = gw_action_string(v, "maskBase64");
        if (mbedtls_base64_decode(r->value, 24, &a, (const unsigned char *)value, strlen(value)) ||
            mbedtls_base64_decode(r->mask, 24, &b, (const unsigned char *)mask, strlen(mask)) ||
            !a || a != b)
            return ESP_ERR_INVALID_ARG;
        r->length = (uint8_t)a;
        p->rule_count++;
    }
    return ESP_OK;
}
static void replace(cJSON *o, const char *k, cJSON *v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddItemToObject(o, k, v);
}
static esp_err_t digest(const cJSON *o, char hex[65]) {
    char *raw = cJSON_PrintUnformatted(o);
    if (!raw)
        return ESP_ERR_NO_MEM;
    uint8_t hash[32];
    int rc = mbedtls_sha256((const unsigned char *)raw, strlen(raw), hash, 0);
    free(raw);
    if (rc)
        return ESP_FAIL;
    gw_action_hex_encode(hash, 32, hex);
    return ESP_OK;
}
/* This is the execution journal, not a replacement for cloud's durable task
 * history. The cloud gate must replay old completed tasks before this API. */
static esp_err_t journal_start(const char *key, const char *identity, const cJSON *request,
                               cJSON **journal, cJSON **details) {
    char hash[65];
    esp_err_t e = digest(request, hash);
    if (e != ESP_OK)
        return e;
    cJSON *j = gw_store_get_json(key);
    if (j) {
        if (!strcmp(gw_action_string(j, "identity"), identity)) {
            if (strcmp(gw_action_string(j, "digest"), hash)) {
                cJSON_Delete(j);
                return ESP_ERR_INVALID_ARG;
            }
            if (details)
                *details = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "details"), true);
            bool done = !strcmp(gw_action_string(j, "phase"), "DONE");
            const cJSON *rc = cJSON_GetObjectItemCaseSensitive(j, "result");
            e = done && cJSON_IsNumber(rc) ? (esp_err_t)rc->valueint : ESP_ERR_TIMEOUT;
            cJSON_Delete(j);
            return e;
        }
        if (strcmp(gw_action_string(j, "phase"), "DONE")) {
            cJSON_Delete(j);
            return ESP_ERR_NOT_FINISHED;
        }
        cJSON_Delete(j);
    }
    j = cJSON_CreateObject();
    if (!j)
        return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "identity", identity);
    cJSON_AddStringToObject(j, "digest", hash);
    cJSON_AddStringToObject(j, "phase", "INTENT");
    cJSON_AddItemToObject(j, "request", cJSON_Duplicate(request, true));
    e = gw_store_put_json(key, j);
    if (e != ESP_OK) {
        cJSON_Delete(j);
        return e;
    }
    *journal = j;
    return ESP_OK;
}
static esp_err_t journal_finish(const char *key, cJSON *j, esp_err_t result, const cJSON *details) {
    replace(j, "phase",
            cJSON_CreateString(
                result == ESP_ERR_TIMEOUT || result == ESP_ERR_NOT_FINISHED ? "UNKNOWN" : "DONE"));
    replace(j, "result", cJSON_CreateNumber(result));
    if (details)
        replace(j, "details", cJSON_Duplicate(details, true));
    esp_err_t e;
    if (result == ESP_OK && !strncmp(key, "actions/config/", 15)) {
        char committed[80];
        snprintf(committed, sizeof(committed), "actions/target/%s", key + 15);
        char *raw = cJSON_PrintUnformatted(j);
        if (!raw)
            e = ESP_ERR_NO_MEM;
        else {
            gw_store_op_t ops[2] = {{.key = key, .data = raw, .size = strlen(raw) + 1},
                                    {.key = committed, .data = raw, .size = strlen(raw) + 1}};
            e = gw_store_transaction(ops, 2);
            free(raw);
        }
    } else
        e = gw_store_put_json(key, j);
    cJSON_Delete(j);
    return e == ESP_OK ? result : ESP_ERR_TIMEOUT;
}
esp_err_t gw_actions_configure(const cJSON *patch, const cJSON *complete, uint64_t revision,
                               uint64_t deadline, cJSON **details) {
    if (!action_lock || !details || !revision || deadline <= gw_core_mono_ms())
        return ESP_ERR_INVALID_ARG;
    *details = NULL;
    if (xSemaphoreTake(action_lock, 0) != pdTRUE)
        return ESP_ERR_NOT_FINISHED;
    const cJSON *radios = cJSON_GetObjectItemCaseSensitive(patch, "radios"), *radio = NULL,
                *target = NULL;
    bool esp = false, adv = false, radio_change = true;
    cJSON *upload = NULL;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(radios) && cJSON_GetArraySize(patch) == 1 &&
        cJSON_GetArraySize(radios) == 1) {
        radio = radios->child;
        esp = !strcmp(radio->string, "esp32");
        if (!esp && strcmp(radio->string, "nrf54"))
            goto done;
        target = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(complete, "radios"), radio->string);
        adv = cJSON_GetObjectItemCaseSensitive(radio, "advertising") != NULL;
        if (adv && cJSON_GetArraySize(radio) != 1)
            goto done;
    } else if (!radios) {
        radio = patch;
        const cJSON *field;
        cJSON_ArrayForEach(field, patch) {
            if (strcmp(field->string, "scan") && strcmp(field->string, "filters") &&
                strcmp(field->string, "upload")) goto done;
        }
        radio_change = cJSON_GetObjectItemCaseSensitive(patch, "scan") ||
                       cJSON_GetObjectItemCaseSensitive(patch, "filters");
        target = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(complete, "radios"), "nrf54");
        const cJSON *upload_patch = cJSON_GetObjectItemCaseSensitive(patch, "upload");
        if (upload_patch) {
            if (!cJSON_IsObject(upload_patch)) goto done;
            upload = gw_capture_upload_configuration();
            if (!upload) { e = ESP_ERR_NO_MEM; goto done; }
            cJSON_ArrayForEach(field, upload_patch) {
                if (!strcmp(field->string, "batchMaxRecords") || !strcmp(field->string, "flushIntervalMs"))
                    replace(upload, field->string, cJSON_Duplicate(field, true));
                else goto done;
            }
            if (!gw_capture_upload_valid(upload)) goto done;
        }
        if (!radio_change && !upload) goto done;
    } else
        goto done;
    if (radio_change && !cJSON_IsObject(target))
        goto done;
    char key[64], identity[32];
    snprintf(key, sizeof(key), "actions/config/%s/%s", radio_change ? (esp ? "esp32" : "nrf54") : "gateway",
             radio_change ? (adv ? "advertising" : "scan") : "upload");
    snprintf(identity, sizeof(identity), "%llu", (unsigned long long)revision);
    cJSON *old = gw_store_get_json(key);
    if (old) {
        uint64_t previous = strtoull(gw_action_string(old, "identity"), NULL, 10);
        cJSON_Delete(old);
        if (revision < previous) {
            e = ESP_ERR_INVALID_STATE;
            goto done;
        }
    }
    cJSON *j = NULL;
    cJSON *request = cJSON_CreateObject();
    if (!request) { e = ESP_ERR_NO_MEM; goto done; }
    if (radio_change) cJSON_AddItemToObject(request, "radioTarget", cJSON_Duplicate(target, true));
    if (upload) cJSON_AddItemToObject(request, "upload", cJSON_Duplicate(upload, true));
    e = journal_start(key, identity, request, &j, details);
    cJSON_Delete(request);
    if (e != ESP_OK || !j)
        goto done;
    e = gw_core_maintenance_acquire(key);
    if (e != ESP_OK) {
        e = journal_finish(key, j, e, *details);
        goto done;
    }
    if (!radio_change) e = ESP_OK;
    else if (esp)
        e = gw_ble_configure(target, adv, revision, deadline, details);
    else {
        e = adv ? gw_nrf_advertise(cJSON_GetObjectItemCaseSensitive(target, "advertising"),
                                   revision, (int64_t)deadline)
                : gw_nrf_scan(target, revision, (int64_t)deadline);
        if (e == ESP_OK && !adv)
            gw_capture_radio_enabled(
                0, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                       cJSON_GetObjectItemCaseSensitive(target, "scan"), "enabled")));
    }
    if (e == ESP_OK && upload) {
        e = gw_capture_configure_upload(upload);
        if (e != ESP_OK && radio_change) e = ESP_ERR_TIMEOUT;
    }
    if (e == ESP_OK && !*details) {
        *details = cJSON_CreateObject();
        if (radio_change) cJSON_AddStringToObject(*details, "radio", esp ? "ESP32" : "NRF54");
        cJSON_AddStringToObject(*details, "group", radio_change ? (adv ? "advertising" : "scan") : "upload");
    }
    if (e == ESP_OK && upload) cJSON_AddItemToObject(*details, "upload", gw_capture_upload_configuration());
    e = journal_finish(key, j, e, *details);
    if (e != ESP_ERR_TIMEOUT && e != ESP_ERR_NOT_FINISHED)
        gw_core_maintenance_release(key);
done:
    cJSON_Delete(upload);
    xSemaphoreGive(action_lock);
    return e;
}
esp_err_t gw_actions_execute(const cJSON *command, uint64_t deadline, cJSON **details) {
    if (!action_lock || !details || !cJSON_IsObject(command) || deadline <= gw_core_mono_ms())
        return ESP_ERR_INVALID_ARG;
    *details = NULL;
    if (xSemaphoreTake(action_lock, 0) != pdTRUE)
        return ESP_ERR_NOT_FINISHED;
    const char *type = gw_action_string(command, "type"), *id = gw_action_string(command, "taskId");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "parameters");
    esp_err_t e = ESP_ERR_NOT_SUPPORTED;
    if (!strcmp(type, "DIAGNOSTICS")) {
        const char *profile = gw_action_string(params, "profile");
        if (!strcmp(profile, "NETWORK")) {
            *details = gw_core_state();
            if (!*details) { e = ESP_ERR_NO_MEM; goto done; }
            const char *scan = gw_action_string(params, "wifiScan");
            if (*scan && strcmp(scan, "OFF")) {
                if (strcmp(scan, "PASSIVE") && strcmp(scan, "ACTIVE")) {
                    e = ESP_ERR_INVALID_ARG; goto done;
                }
                cJSON *aps = NULL;
                e = gw_core_wifi_scan(!strcmp(scan, "ACTIVE"), deadline, &aps);
                if (e != ESP_OK) { cJSON_Delete(aps); goto done; }
                cJSON *ap;
                cJSON_ArrayForEach(ap, aps) {
                    cJSON *rssi = cJSON_DetachItemFromObjectCaseSensitive(ap, "rssi");
                    cJSON *security = cJSON_DetachItemFromObjectCaseSensitive(ap, "security");
                    cJSON_AddItemToObject(ap, "rssiDbm", rssi);
                    cJSON_AddItemToObject(ap, "authentication", security);
                }
                cJSON_AddItemToObject(*details, "accessPoints", aps);
            }
            e = ESP_OK; goto done;
        }
        if (strcmp(profile, "BLE")) { e = ESP_ERR_NOT_SUPPORTED; goto done; }
        *details = gw_ble_diagnostics();
        cJSON_AddItemToObject(*details, "nrf54", gw_nrf_diagnostics((int64_t)deadline));
        e = *details ? ESP_OK : ESP_ERR_NO_MEM;
        goto done;
    }
    if (!*id || strlen(id) > 128) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
    }
    cJSON *j = NULL;
    e = journal_start("actions/current", id, command, &j, details);
    if (e != ESP_OK || !j)
        goto done;
    if (!strcmp(type, "FIRMWARE_UPGRADE"))
        e = gw_ota_execute(command, deadline, details);
    else if (!strncmp(type, "GATT_", 5)) {
        e = gw_core_maintenance_acquire(id);
        if (e == ESP_OK) {
            e = !strcmp(gw_action_string(params, "radio"), "ESP32")
                    ? gw_ble_execute(command, deadline, details)
                    : gw_gatt_nrf_execute(command, deadline, details);
            if (e != ESP_ERR_TIMEOUT && e != ESP_ERR_NOT_FINISHED)
                gw_core_maintenance_release(id);
        }
    } else
        e = ESP_ERR_NOT_SUPPORTED;
    e = journal_finish("actions/current", j, e, *details);
done:
    xSemaphoreGive(action_lock);
    return e;
}
esp_err_t gw_actions_recover(const cJSON *command, uint64_t deadline, cJSON **details) {
    if (!details || !action_lock)
        return ESP_ERR_INVALID_ARG;
    *details = NULL;
    if (xSemaphoreTake(action_lock, 0) != pdTRUE)
        return ESP_ERR_NOT_FINISHED;
    cJSON *j = gw_store_get_json("actions/current");
    esp_err_t e = ESP_ERR_TIMEOUT;
    char hash[65];
    if (!j || strcmp(gw_action_string(j, "identity"), gw_action_string(command, "taskId")) ||
        digest(command, hash) != ESP_OK || strcmp(hash, gw_action_string(j, "digest")))
        goto done;
    if (!strcmp(gw_action_string(j, "phase"), "DONE")) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(j, "result");
        e = cJSON_IsNumber(r) ? r->valueint : ESP_ERR_TIMEOUT;
        *details = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "details"), true);
    } else if (!strcmp(gw_action_string(command, "type"), "FIRMWARE_UPGRADE")) {
        e = gw_ota_recover(command, deadline, details);
        if (e != ESP_ERR_NOT_FINISHED) {
            e = journal_finish("actions/current", j, e, *details);
            j = NULL;
        }
    } else if (!strncmp(gw_action_string(command, "type"), "GATT_", 5) &&
               strcmp(gw_action_string(cJSON_GetObjectItemCaseSensitive(command, "parameters"),
                                       "radio"),
                      "ESP32")) {
        e = gw_gatt_nrf_recover(command, deadline, details);
        if (e != ESP_ERR_TIMEOUT && e != ESP_ERR_NOT_FINISHED) {
            gw_core_maintenance_release(gw_action_string(command, "taskId"));
            e = journal_finish("actions/current", j, e, *details);
            j = NULL;
        }
    }
done:
    cJSON_Delete(j);
    xSemaphoreGive(action_lock);
    return e;
}
static void restore_radios(void *arg) {
    (void)arg;
    uint8_t restored_boot[16] = {0};
    bool esp_restored = false, nrf_restored = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (xSemaphoreTake(action_lock, 0) != pdTRUE)
            continue;
        if (gw_core_maintenance_acquire("radio-restore") != ESP_OK) {
            xSemaphoreGive(action_lock);
            continue;
        }
        uint8_t boot[16];
        bool online = gw_nrf_identity(boot, NULL, NULL);
        if (online && memcmp(boot, restored_boot, 16))
            nrf_restored = false;
        for (unsigned radio = 0; radio < 2; radio++) {
            if ((radio == 0 && (!online || nrf_restored)) || (radio == 1 && esp_restored))
                continue;
            bool success = true;
            for (unsigned adv = 0; adv < 2; adv++) {
                char key[64];
                snprintf(key, sizeof(key), "actions/target/%s/%s", radio ? "esp32" : "nrf54",
                         adv ? "advertising" : "scan");
                cJSON *j = gw_store_get_json(key);
                if (!j)
                    continue;
                const cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result"),
                            *request = cJSON_GetObjectItemCaseSensitive(j, "request"),
                            *target = cJSON_GetObjectItemCaseSensitive(request, "radioTarget");
                if (strcmp(gw_action_string(j, "phase"), "DONE") || !cJSON_IsNumber(result) ||
                    result->valueint != ESP_OK) {
                    cJSON_Delete(j);
                    continue;
                }
                uint64_t revision = strtoull(gw_action_string(j, "identity"), NULL, 10),
                         deadline = gw_core_mono_ms() + 2500;
                cJSON *details = NULL;
                esp_err_t e;
                if (radio)
                    e = gw_ble_configure(target, adv != 0, revision, deadline, &details);
                else {
                    e = adv ? gw_nrf_advertise(
                                  cJSON_GetObjectItemCaseSensitive(target, "advertising"), revision,
                                  (int64_t)deadline)
                            : gw_nrf_scan(target, revision, (int64_t)deadline);
                    if (e == ESP_OK && !adv)
                        gw_capture_radio_enabled(
                            0, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                                   cJSON_GetObjectItemCaseSensitive(target, "scan"), "enabled")));
                }
                cJSON_Delete(details);
                cJSON_Delete(j);
                if (e != ESP_OK)
                    success = false;
            }
            if (radio)
                esp_restored = success;
            else {
                nrf_restored = success;
                if (success)
                    memcpy(restored_boot, boot, 16);
            }
        }
        gw_core_maintenance_release("radio-restore");
        xSemaphoreGive(action_lock);
    }
}
esp_err_t gw_actions_init(void) {
    if (action_lock)
        return ESP_ERR_INVALID_STATE;
    action_lock = xSemaphoreCreateMutex();
    if (!action_lock)
        return ESP_ERR_NO_MEM;
    esp_err_t e = gw_ota_init();
    if (e != ESP_OK)
        return e;
    e = gw_gatt_nrf_init();
    if (e != ESP_OK)
        return e;
    e = gw_ble_init();
    if (e != ESP_OK)
        return e;
    cJSON *current = gw_store_get_json("actions/current");
    if (current && strcmp(gw_action_string(current, "phase"), "DONE"))
        gw_core_maintenance_acquire(gw_action_string(current, "identity"));
    cJSON_Delete(current);
    for (unsigned radio = 0; radio < 2; radio++)
        for (unsigned adv = 0; adv < 2; adv++) {
            char key[64];
            snprintf(key, sizeof(key), "actions/config/%s/%s", radio ? "esp32" : "nrf54",
                     adv ? "advertising" : "scan");
            current = gw_store_get_json(key);
            if (current && strcmp(gw_action_string(current, "phase"), "DONE"))
                gw_core_maintenance_acquire(key);
            cJSON_Delete(current);
        }
    return xTaskCreate(restore_radios, "radio_restore", 8192, NULL, 3, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
