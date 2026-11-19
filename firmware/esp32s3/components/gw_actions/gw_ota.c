#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_actions_internal.h"
#include "gw_capture.h"
#include "gw_http.h"
#include "gw_nrf.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "monocypher-ed25519.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OTA_KEY "ota/active"
static esp_timer_handle_t trial_timer;
static uint64_t trial_deadline;
static bool recovery_authorized = true;
static void set(cJSON *j, const char *k, cJSON *v) {
    cJSON_DeleteItemFromObjectCaseSensitive(j, k);
    cJSON_AddItemToObject(j, k, v);
}
static esp_err_t phase(cJSON *j, const char *value) {
    set(j, "phase", cJSON_CreateString(value));
    return gw_store_put_json(OTA_KEY, j);
}
static bool safe_id(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 128)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}
static int64_t expires_seconds(const char *s) {
    int y, m, d, h, minute, second, used = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &m, &d, &h, &minute, &second, &used) != 6 ||
        used != 19)
        return -1;
    static const unsigned md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 ||
        d > (int)(md[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) || h < 0 ||
        h > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
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
        if (p[3] != ':' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9' || p[4] < '0' ||
            p[4] > '9' || p[5] < '0' || p[5] > '9')
            return -1;
        int zh = 10 * (p[1] - '0') + p[2] - '0', zm = 10 * (p[4] - '0') + p[5] - '0';
        if (zh > 23 || zm > 59)
            return -1;
        zone = (zh * 60 + zm) * 60 * (*p == '+' ? 1 : -1);
    } else
        return -1;
    y -= m <= 2;
    int era = y / 400;
    unsigned yo = y - era * 400, mp = m + (m > 2 ? -3 : 9), day = (153 * mp + 2) / 5 + d - 1,
             doe = yo * 365 + yo / 4 - yo / 100 + day;
    return ((int64_t)era * 146097 + doe - 719468) * 86400 + h * 3600 + minute * 60 + second - zone;
}
static bool task_budget(const cJSON *command, const cJSON *j, uint64_t *deadline) {
    int64_t expires = expires_seconds(gw_action_string(command, "expiresAt")),
            now = (int64_t)time(NULL);
    uint64_t mono = gw_core_mono_ms();
    if (!gw_core_time_trusted() || expires <= now)
        return false;
    uint64_t limit = mono + (uint64_t)(expires - now) * 1000;
    if (*deadline > limit)
        *deadline = limit;
    uint8_t boot[16];
    char hex[33];
    gw_capture_esp_boot(boot);
    gw_action_hex_encode(boot, 16, hex);
    if (j && !strcmp(hex, gw_action_string(j, "espBoot"))) {
        uint64_t original = strtoull(gw_action_string(j, "deadlineMono"), NULL, 10);
        if (original && *deadline > original)
            *deadline = original;
    }
    return *deadline > mono + 100;
}
static bool member(const cJSON *j, const char *key, const char *value, bool required) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, key), *v;
    if (!list)
        return !required;
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > 128)
        return false;
    if (!cJSON_GetArraySize(list))
        return !required;
    bool found = false;
    cJSON_ArrayForEach(v, list) {
        if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) > 128)
            return false;
        if (!strcmp(v->valuestring, value))
            found = true;
    }
    return found;
}
static bool manifest_shape(const char *raw) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (*p == '\\')
                escaped = true;
            else if (*p == '"')
                quoted = false;
            else if (*p < 0x20)
                return false;
        } else if (*p == '"')
            quoted = true;
        else if (*p == '{' || *p == '[') {
            if (++depth > 8)
                return false;
        } else if (*p == '}' || *p == ']') {
            if (!depth)
                return false;
            depth--;
        }
    }
    return !quoted && !depth;
}
static bool unique_keys(const cJSON *j, unsigned depth) {
    if (depth > 8)
        return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, j) {
        if (cJSON_IsObject(j)) {
            if (!a->string)
                return false;
            for (b = a->next; b; b = b->next)
                if (b->string && !strcmp(a->string, b->string))
                    return false;
        }
        if ((cJSON_IsObject(a) || cJSON_IsArray(a)) && !unique_keys(a, depth + 1))
            return false;
    }
    return true;
}
/* Exact raw manifest bytes are authenticated with a factory-provisioned
 * release key. The network cannot install or replace this trust anchor. */
static esp_err_t verify_manifest(const cJSON *command, const cJSON *response, cJSON **out) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(command, "parameters");
    const char *component = gw_action_string(p, "component");
    if (!*component)
        component = "GATEWAY";
    cJSON *m = NULL;
    esp_err_t e;
    const char *raw = gw_action_string(response, "manifest"),
               *signature = gw_action_string(response, "signature");
    size_t length = strlen(raw);
    if (!length || length > 65536 || strstr(raw, "\\u0000") || !manifest_shape(raw)) {
        e = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    const char *end = NULL;
    m = cJSON_ParseWithLengthOpts(raw, length + 1, &end, true);
    if (!cJSON_IsObject(m) || !unique_keys(m, 0)) {
        e = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }
    char *keys = malloc(8192);
    if (!keys) {
        e = ESP_ERR_NO_MEM;
        goto done;
    }
    if (gw_factory_get_string("artifact_keys", keys, 8192) != ESP_OK) {
        free(keys);
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    cJSON *trusted = cJSON_Parse(keys);
    memset(keys, 0, 8192);
    free(keys);
    const char *encoded = gw_action_string(trusted, gw_action_string(m, "keyId"));
    uint8_t public_key[32], sig[64];
    size_t pn = 0, sn = 0;
    bool decoded = mbedtls_base64_decode(public_key, sizeof(public_key), &pn,
                                         (const unsigned char *)encoded, strlen(encoded)) == 0 &&
                   pn == 32 &&
                   mbedtls_base64_decode(sig, sizeof(sig), &sn, (const unsigned char *)signature,
                                         strlen(signature)) == 0 &&
                   sn == 64;
    cJSON_Delete(trusted);
    if (!decoded || crypto_ed25519_check(sig, public_key, (const uint8_t *)raw, length)) {
        e = ESP_ERR_INVALID_CRC;
        goto done;
    }
    const char *signed_component = gw_action_string(m, "component");
    if (!*signed_component)
        signed_component = "GATEWAY";
    char model[129], hardware[129], current[65];
    uint8_t hash[32];
    if (gw_factory_get_string("model", model, sizeof(model)) != ESP_OK ||
        gw_factory_get_string("hardware", hardware, sizeof(hardware)) != ESP_OK) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (!strcmp(component, "NRF54")) {
        if (!gw_nrf_identity(NULL, NULL, current)) {
            e = ESP_ERR_INVALID_STATE;
            goto done;
        }
    } else
        snprintf(current, sizeof(current), "%s", esp_app_get_description()->version);
    if (strcmp(gw_action_string(m, "kind"), "FIRMWARE") || strcmp(component, signed_component) ||
        !gw_action_hex(gw_action_string(m, "sha256"), hash, 32) || !*gw_action_string(m, "name") ||
        strlen(gw_action_string(m, "name")) > 128 || !*gw_action_string(m, "version") ||
        strlen(gw_action_string(m, "version")) > 64 ||
        strcmp(gw_action_string(m, "version"), gw_action_string(p, "expectedFirmwareVersion")) ||
        !member(m, "compatibleModels", model, true) ||
        !member(m, "hardwareVersions", hardware, false) ||
        !member(m, "fromFirmwareVersions", current, false) ||
        (!strcmp(component, "NRF54") &&
         strcmp(gw_action_string(m, "imageFormat"), "MCUBOOT_SIGNED_BIN"))) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
    }
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(m, "sizeBytes"),
                *format = cJSON_GetObjectItemCaseSensitive(m, "imageFormat");
    /* Older GATEWAY manifests may omit these fields. Explicit signed metadata
     * is authoritative, including its type, and must never be silently ignored. */
    if ((size && (!cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
                  size->valuedouble < 32 || size->valuedouble > UINT32_MAX ||
                  floor(size->valuedouble) != size->valuedouble)) ||
        (!strcmp(component, "GATEWAY") && format &&
         (!cJSON_IsString(format) || strcmp(format->valuestring, "ESP_IDF_APP_BIN")))) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
    }
    *out = m;
    m = NULL;
    e = ESP_OK;
done:
    cJSON_Delete(m);
    return e;
}
static esp_err_t manifest(const cJSON *command, uint64_t deadline, cJSON **out) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(command, "parameters");
    const char *artifact = gw_action_string(p, "artifactId"),
               *component = gw_action_string(p, "component");
    if (!safe_id(artifact) ||
        (*component && strcmp(component, "NRF54") && strcmp(component, "GATEWAY")))
        return ESP_ERR_INVALID_ARG;
    char path[192];
    snprintf(path, sizeof(path), "/api/v1/artifacts/%s/verification", artifact);
    cJSON *response = NULL;
    esp_err_t e = gw_http_json(path, "GET", NULL, NULL, deadline, &response);
    if (e == ESP_OK)
        e = verify_manifest(command, response, out);
    cJSON_Delete(response);
    return e;
}
typedef struct {
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    bool gateway;
    mbedtls_sha256_context hash;
    size_t offset, total;
    size_t limit;
    const cJSON *journal;
    uint64_t deadline;
    uint8_t pending[16];
    size_t pending_length;
} download_t;
static esp_err_t download_sink(const uint8_t *data, size_t n, void *context) {
    download_t *d = context;
    if (!task_budget(d->journal, d->journal, &d->deadline))
        return ESP_ERR_TIMEOUT;
    if (n > d->limit - d->total)
        return ESP_ERR_INVALID_SIZE;
    if (mbedtls_sha256_update(&d->hash, data, n))
        return ESP_FAIL;
    d->total += n;
    if (d->gateway) {
        if (!task_budget(d->journal, d->journal, &d->deadline))
            return ESP_ERR_TIMEOUT;
        esp_err_t e = esp_ota_write(d->handle, data, n);
        if (e != ESP_OK)
            return e;
        return task_budget(d->journal, d->journal, &d->deadline) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    while (n) {
        size_t take = 16 - d->pending_length;
        if (take > n)
            take = n;
        memcpy(d->pending + d->pending_length, data, take);
        d->pending_length += take;
        data += take;
        n -= take;
        if (d->pending_length == 16) {
            if (!task_budget(d->journal, d->journal, &d->deadline))
                return ESP_ERR_TIMEOUT;
            esp_err_t e = esp_partition_write(d->partition, d->offset, d->pending, 16);
            if (e != ESP_OK)
                return e;
            d->offset += 16;
            d->pending_length = 0;
        }
    }
    return task_budget(d->journal, d->journal, &d->deadline) ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t partition_hash(const esp_partition_t *p, size_t length, uint8_t hash[32]) {
    if (!p || length > p->size)
        return ESP_ERR_INVALID_SIZE;
    uint8_t *buffer = malloc(4096);
    if (!buffer)
        return ESP_ERR_NO_MEM;
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    esp_err_t e = mbedtls_sha256_starts(&ctx, 0) ? ESP_FAIL : ESP_OK;
    for (size_t offset = 0; e == ESP_OK && offset < length;) {
        size_t n = length - offset;
        if (n > 4096)
            n = 4096;
        e = esp_partition_read(p, offset, buffer, n);
        if (e == ESP_OK && mbedtls_sha256_update(&ctx, buffer, n))
            e = ESP_FAIL;
        offset += n;
    }
    if (e == ESP_OK && mbedtls_sha256_finish(&ctx, hash))
        e = ESP_FAIL;
    mbedtls_sha256_free(&ctx);
    free(buffer);
    return e;
}
static esp_err_t download(cJSON *j, uint64_t deadline) {
    if (!task_budget(j, j, &deadline))
        return ESP_ERR_TIMEOUT;
    bool gateway = !strcmp(gw_action_string(j, "component"), "GATEWAY");
    const esp_partition_t *p =
        gateway ? esp_ota_get_next_update_partition(NULL)
                : esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "nrf_image");
    if (!p)
        return ESP_ERR_NOT_FOUND;
    const cJSON *signed_size = cJSON_GetObjectItemCaseSensitive(j, "signedSize");
    if (signed_size && (!cJSON_IsNumber(signed_size) || !isfinite(signed_size->valuedouble) ||
                        signed_size->valuedouble < 32 || signed_size->valuedouble > p->size ||
                        floor(signed_size->valuedouble) != signed_size->valuedouble))
        return ESP_ERR_INVALID_SIZE;
    download_t d = {.partition = p, .gateway = gateway, .journal = j, .deadline = deadline,
                    .limit = signed_size ? (size_t)signed_size->valuedouble : p->size};
    mbedtls_sha256_init(&d.hash);
    esp_err_t e = phase(j, "DOWNLOADING");
    if (e != ESP_OK)
        goto done;
    if (!task_budget(j, j, &d.deadline)) {
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    e = gateway ? esp_ota_begin(p, signed_size ? d.limit : OTA_SIZE_UNKNOWN, &d.handle)
                : esp_partition_erase_range(p, 0, p->size);
    if (e != ESP_OK)
        goto done;
    if (!task_budget(j, j, &d.deadline)) {
        e = ESP_ERR_TIMEOUT;
        goto abort;
    }
    if (mbedtls_sha256_starts(&d.hash, 0)) {
        e = ESP_FAIL;
        goto abort;
    }
    char path[192];
    snprintf(path, sizeof(path), "/api/v1/artifacts/%s/content", gw_action_string(j, "artifactId"));
    size_t received = 0;
    e = gw_http_stream(path, d.deadline, d.limit, download_sink, &d, &received);
    if (e != ESP_OK)
        goto abort;
    uint8_t hash[32], expected[32];
    if (received != d.total || d.total < 32 || (signed_size && d.total != d.limit) ||
        mbedtls_sha256_finish(&d.hash, hash) ||
        !gw_action_hex(gw_action_string(j, "sha256"), expected, 32) || memcmp(hash, expected, 32)) {
        e = ESP_ERR_INVALID_CRC;
        goto abort;
    }
    if (!task_budget(j, j, &d.deadline)) {
        e = ESP_ERR_TIMEOUT;
        goto abort;
    }
    if (gateway) {
        e = esp_ota_end(d.handle);
        d.handle = 0;
        if (e != ESP_OK)
            goto done;
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(p, &desc) != ESP_OK ||
            !memchr(desc.version, 0, sizeof(desc.version)) ||
            strcmp(desc.version, gw_action_string(j, "version"))) {
            e = ESP_ERR_INVALID_VERSION;
            goto done;
        }
    } else if (d.pending_length) {
        memset(d.pending + d.pending_length, 0xff, 16 - d.pending_length);
        e = esp_partition_write(p, d.offset, d.pending, 16);
        if (e != ESP_OK)
            goto done;
    }
    e = partition_hash(p, d.total, hash);
    if (e != ESP_OK || memcmp(hash, expected, 32)) {
        e = ESP_ERR_INVALID_CRC;
        goto done;
    }
    if (!task_budget(j, j, &d.deadline)) {
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    set(j, "size", cJSON_CreateNumber((double)d.total));
    set(j, "partition", cJSON_CreateString(p->label));
    e = phase(j, "VERIFIED_ARTIFACT");
    goto done;
abort:
    if (gateway && d.handle)
        esp_ota_abort(d.handle);
done:
    mbedtls_sha256_free(&d.hash);
    return e;
}
static esp_err_t command(uint8_t opcode, const void *args, size_t size, uint8_t *data,
                         size_t *length, uint64_t deadline) {
    uint8_t status = 255;
    esp_err_t e = gw_nrf_command(opcode, args, size, &status, data, length, (int64_t)deadline);
    if (e == ESP_OK && status)
        e = status == GW_BUSY          ? ESP_ERR_NOT_FINISHED
            : status == GW_UNSUPPORTED ? ESP_ERR_NOT_SUPPORTED
                                       : ESP_FAIL;
    return e;
}
static esp_err_t dfu_status(uint8_t status[62], uint64_t deadline) {
    size_t n = 62;
    esp_err_t e = command(0x33, NULL, 0, status, &n, deadline);
    return e == ESP_OK && n != 62 ? ESP_ERR_INVALID_RESPONSE : e;
}
static esp_err_t nrf_health(uint64_t deadline) {
    uint8_t status[71];
    size_t n = sizeof(status);
    esp_err_t e = command(3, NULL, 0, status, &n, deadline);
    return e == ESP_OK && n != 71 ? ESP_ERR_INVALID_RESPONSE : e;
}
static cJSON *result(cJSON *j, const uint8_t boot[16]) {
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "component", gw_action_string(j, "component"));
    cJSON_AddStringToObject(o, "firmwareVersion", gw_action_string(j, "version"));
    cJSON_AddStringToObject(o, "imageSha256", gw_action_string(j, "sha256"));
    cJSON_AddBoolToObject(o, "bootConfirmed", true);
    if (boot) {
        char hex[33];
        gw_action_hex_encode(boot, 16, hex);
        cJSON_AddStringToObject(o, "nrfBootId", hex);
    }
    return o;
}
static bool session_matches(const uint8_t status[62], cJSON *j) {
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
    uint8_t hash[32];
    return cJSON_IsNumber(size) && size->valuedouble >= 32 && size->valuedouble <= UINT32_MAX &&
           gw_get_u64(status) == strtoull(gw_action_string(j, "session"), NULL, 10) &&
           gw_get_u32(status + 14) == (uint32_t)size->valuedouble &&
           gw_action_hex(gw_action_string(j, "sha256"), hash, 32) && !memcmp(status + 26, hash, 32);
}
static esp_err_t nrf_trial(cJSON *j, uint64_t deadline, cJSON **details) {
    uint8_t old_boot[16], boot[16], status[62], args[36];
    char version[64];
    if (!gw_action_hex(gw_action_string(j, "previousBoot"), old_boot, 16))
        return ESP_ERR_INVALID_STATE;
    while (gw_core_mono_ms() < deadline) {
        if (gw_nrf_identity(boot, NULL, version) && memcmp(boot, old_boot, 16))
            break;
        (void)gw_nrf_rehandshake(
            (int64_t)(gw_core_mono_ms() + 2000 < deadline ? gw_core_mono_ms() + 2000 : deadline));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!gw_nrf_identity(boot, NULL, version) || !memcmp(boot, old_boot, 16))
        return ESP_ERR_NOT_FINISHED;
    esp_err_t e = dfu_status(status, deadline);
    if (e != ESP_OK)
        return e;
    if (status[8] != 0)
        return ESP_ERR_INVALID_STATE;
    if (strcmp(version, gw_action_string(j, "version"))) {
        if (!status[9])
            return ESP_ERR_NOT_FINISHED;
        phase(j, "ROLLED_BACK");
        return ESP_ERR_INVALID_VERSION;
    }
    if (!status[9]) {
        uint32_t remaining = gw_get_u32(status + 58);
        if (remaining < 500)
            return ESP_ERR_TIMEOUT;
        uint64_t budget = gw_core_mono_ms() + remaining - 200;
        if (deadline > budget)
            deadline = budget;
    }
    if (!status[9] && !recovery_authorized)
        return ESP_ERR_NOT_FINISHED;
    if ((e = nrf_health(deadline)) != ESP_OK)
        return e;
    gw_put_u32(args, (uint32_t)cJSON_GetObjectItemCaseSensitive(j, "size")->valuedouble);
    gw_action_hex(gw_action_string(j, "sha256"), args + 4, 32);
    size_t n = 0;
    if ((e = command(0x37, args, sizeof(args), NULL, &n, deadline)) != ESP_OK)
        return e;
    if ((e = phase(j, "CONFIRMING")) != ESP_OK)
        return e;
    if (!status[9]) {
        if (!task_budget(j, j, &deadline))
            return ESP_ERR_NOT_FINISHED;
        n = 0;
        if ((e = command(0x36, NULL, 0, NULL, &n, deadline)) != ESP_OK)
            return e;
    }
    if ((e = dfu_status(status, deadline)) != ESP_OK || !status[9])
        return e == ESP_OK ? ESP_ERR_INVALID_RESPONSE : e;
    char hex[33];
    gw_action_hex_encode(boot, 16, hex);
    set(j, "confirmedBoot", cJSON_CreateString(hex));
    if ((e = phase(j, "CONFIRMED")) != ESP_OK)
        return ESP_ERR_TIMEOUT;
    *details = result(j, boot);
    return *details ? ESP_OK : ESP_ERR_NO_MEM;
}
static esp_err_t nrf_transfer(cJSON *j, uint64_t deadline, cJSON **details) {
    uint8_t boot[16], old[16], status[62], hash[32];
    uint32_t caps = 0;
    if (!gw_nrf_identity(boot, &caps, NULL) || !(caps & (1u << 12)))
        return ESP_ERR_NOT_SUPPORTED;
    if (!gw_action_hex(gw_action_string(j, "previousBoot"), old, 16) || memcmp(boot, old, 16))
        return nrf_trial(j, deadline, details);
    esp_err_t e = dfu_status(status, deadline);
    if (e != ESP_OK)
        return e;
    if (!recovery_authorized) {
        if (status[8] == 5)
            return ESP_ERR_NOT_FINISHED;
        if (status[8] == 0) {
            if (!status[9])
                return ESP_ERR_NOT_FINISHED;
            phase(j, "FAILED");
            return ESP_FAIL;
        }
        if (!session_matches(status, j))
            return ESP_ERR_NOT_FINISHED;
        if (status[8] != 4) {
            uint8_t session[8];
            gw_put_u64(session, gw_get_u64(status));
            if ((e = phase(j, "ABORT_INTENT")) != ESP_OK)
                return e;
            size_t n = 62;
            if ((e = command(0x34, session, sizeof(session), status, &n, deadline)) != ESP_OK ||
                n != 62 || status[8] != 4)
                return ESP_ERR_NOT_FINISHED;
        }
        phase(j, "FAILED");
        return ESP_FAIL;
    }
    const char *p = gw_action_string(j, "phase");
    if (status[8] == 0) {
        if (strcmp(p, "VERIFIED_ARTIFACT") && strcmp(p, "BEGIN_INTENT"))
            return ESP_ERR_INVALID_STATE;
        if (!status[9])
            return ESP_ERR_INVALID_STATE;
        uint32_t size = (uint32_t)cJSON_GetObjectItemCaseSensitive(j, "size")->valuedouble;
        if (size > gw_get_u32(status + 18))
            return ESP_ERR_INVALID_SIZE;
        uint8_t begin[44];
        gw_put_u64(begin, strtoull(gw_action_string(j, "session"), NULL, 10));
        gw_put_u32(begin + 8, size);
        gw_action_hex(gw_action_string(j, "sha256"), begin + 12, 32);
        if ((e = phase(j, "BEGIN_INTENT")) != ESP_OK)
            return e;
        size_t n = 62;
        e = command(0x30, begin, sizeof(begin), status, &n, deadline);
        if (e == ESP_ERR_NOT_FINISHED) {
            phase(j, "FAILED");
            return ESP_ERR_INVALID_STATE;
        }
        if (e != ESP_OK)
            return e;
        if (n != 62)
            return ESP_ERR_INVALID_RESPONSE;
    }
    if (!session_matches(status, j))
        return ESP_ERR_INVALID_STATE;
    if (status[8] == 3 || status[8] == 4) {
        if (status[8] == 3) {
            uint8_t session[8];
            gw_put_u64(session, gw_get_u64(status));
            if ((e = phase(j, "ABORT_INTENT")) != ESP_OK)
                return e;
            size_t n = 62;
            if (command(0x34, session, sizeof(session), status, &n, deadline) != ESP_OK ||
                n != 62 || status[8] != 4)
                return ESP_ERR_NOT_FINISHED;
        }
        if (phase(j, "FAILED") != ESP_OK)
            return ESP_ERR_TIMEOUT;
        return ESP_FAIL;
    }
    const esp_partition_t *cache =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "nrf_image");
    size_t size = (size_t)cJSON_GetObjectItemCaseSensitive(j, "size")->valuedouble;
    uint8_t actual[32];
    gw_action_hex(gw_action_string(j, "sha256"), hash, 32);
    if (partition_hash(cache, size, actual) != ESP_OK || memcmp(actual, hash, 32))
        return ESP_ERR_INVALID_CRC;
    if (status[8] == 1) {
        if ((e = phase(j, "TRANSFERRING")) != ESP_OK)
            return e;
        uint8_t chunk[1038];
        gw_put_u64(chunk, strtoull(gw_action_string(j, "session"), NULL, 10));
        uint32_t offset = gw_get_u32(status + 10);
        if (offset > size || (offset && offset < 32))
            return ESP_ERR_INVALID_RESPONSE;
        while (offset < size) {
            if (gw_core_mono_ms() + 100 >= deadline)
                return ESP_ERR_TIMEOUT;
            size_t n = size - offset;
            if (n > 1024)
                n = 1024;
            gw_put_u32(chunk + 8, offset);
            gw_put_u16(chunk + 12, (uint16_t)n);
            if ((e = esp_partition_read(cache, offset, chunk + 14, n)) != ESP_OK)
                return e;
            size_t reply = 62;
            e = command(0x31, chunk, n + 14, status, &reply, deadline);
            if (e != ESP_OK) { /* Query authoritative offset before any retry. */
                if (dfu_status(status, deadline) != ESP_OK)
                    return ESP_ERR_TIMEOUT;
            }
            if (!session_matches(status, j) || status[8] != 1)
                return ESP_ERR_INVALID_RESPONSE;
            uint32_t accepted = gw_get_u32(status + 10);
            if (accepted != offset + n)
                return ESP_ERR_TIMEOUT;
            offset = accepted;
        }
        uint8_t session[8];
        gw_put_u64(session, strtoull(gw_action_string(j, "session"), NULL, 10));
        if ((e = phase(j, "FINISH_INTENT")) != ESP_OK)
            return e;
        size_t n = 62;
        e = command(0x32, session, sizeof(session), status, &n, deadline);
        if (e != ESP_OK)
            return e;
        if (n != 62 || !session_matches(status, j) || status[8] != 2)
            return ESP_ERR_INVALID_RESPONSE;
    }
    if (status[8] == 2) {
        if ((e = phase(j, "READY")) != ESP_OK)
            return e;
        /* FIFO bytes remain owned by nRF until durable capture ACK. Never
         * reboot the controller while its queue still contains records. */
        for (;;) {
            uint8_t s[71];
            size_t n = sizeof(s);
            if ((e = command(3, NULL, 0, s, &n, deadline)) != ESP_OK)
                return e;
            if (n != 71)
                return ESP_ERR_INVALID_RESPONSE;
            if (!gw_get_u16(s + 17) && !gw_get_u32(s + 19))
                break;
            if (gw_core_mono_ms() + 200 >= deadline)
                return ESP_ERR_NOT_FINISHED;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if ((e = phase(j, "REBOOT_INTENT")) != ESP_OK)
            return e;
        if (!task_budget(j, j, &deadline))
            return ESP_ERR_NOT_FINISHED;
        uint8_t session[8];
        gw_put_u64(session, strtoull(gw_action_string(j, "session"), NULL, 10));
        size_t n = 0;
        e = command(0x35, session, sizeof(session), NULL, &n, deadline);
        if (e != ESP_OK && e != ESP_ERR_TIMEOUT)
            return e;
    } else if (status[8] != 5)
        return ESP_ERR_INVALID_STATE;
    return nrf_trial(j, deadline, details);
}
static void trial_expired(void *arg) {
    (void)arg;
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart();
}
esp_err_t gw_ota_init(void) {
    esp_err_t local_error = gw_ota_local_init();
    if (local_error != ESP_OK)
        return local_error;
    cJSON *j = gw_store_get_json(OTA_KEY);
    if (j && strcmp(gw_action_string(j, "phase"), "CONFIRMED") &&
        strcmp(gw_action_string(j, "phase"), "FAILED") &&
        strcmp(gw_action_string(j, "phase"), "ROLLED_BACK"))
        gw_core_maintenance_acquire(gw_action_string(j, "taskId"));
    cJSON_Delete(j);
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY)
        return ESP_OK;
    trial_deadline = gw_core_mono_ms() + 90000;
    const esp_timer_create_args_t args = {.callback = trial_expired, .name = "ota_trial"};
    esp_err_t e = esp_timer_create(&args, &trial_timer);
    if (e != ESP_OK)
        return e;
    return esp_timer_start_once(trial_timer, 90000000);
}
esp_err_t gw_ota_execute(const cJSON *command_json, uint64_t deadline, cJSON **details) {
    recovery_authorized = task_budget(command_json, NULL, &deadline);
    if (!recovery_authorized)
        return ESP_ERR_TIMEOUT;
    const char *id = gw_action_string(command_json, "taskId");
    esp_err_t e = gw_core_maintenance_acquire(id);
    if (e != ESP_OK)
        return e;
    cJSON *m = NULL, *j = NULL;
    e = manifest(command_json, deadline, &m);
    if (e != ESP_OK)
        goto done;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(command_json, "parameters");
    const char *component = gw_action_string(p, "component");
    if (!*component)
        component = "GATEWAY";
    uint8_t boot[16], dfu[62];
    uint32_t caps;
    if (!gw_nrf_identity(boot, &caps, NULL)) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (!strcmp(component, "NRF54") && !(caps & (1u << 12))) {
        e = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    if (caps & (1u << 12)) {
        e = dfu_status(dfu, deadline);
        if (e != ESP_OK || !dfu[9] || (dfu[8] != 0 && dfu[8] != 4)) {
            if (e == ESP_OK)
                e = ESP_ERR_INVALID_STATE;
            goto done;
        }
    }
    j = cJSON_CreateObject();
    if (!j) {
        e = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(j, "taskId", id);
    cJSON_AddStringToObject(j, "component", component);
    cJSON_AddStringToObject(j, "artifactId", gw_action_string(p, "artifactId"));
    cJSON_AddStringToObject(j, "version", gw_action_string(m, "version"));
    cJSON_AddStringToObject(j, "sha256", gw_action_string(m, "sha256"));
    const cJSON *signed_size = cJSON_GetObjectItemCaseSensitive(m, "sizeBytes");
    if (signed_size && !cJSON_AddNumberToObject(j, "signedSize", signed_size->valuedouble)) {
        e = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(j, "expiresAt", gw_action_string(command_json, "expiresAt"));
    uint8_t esp_boot[16];
    char esp_hex[33], mono[24];
    gw_capture_esp_boot(esp_boot);
    gw_action_hex_encode(esp_boot, 16, esp_hex);
    snprintf(mono, sizeof(mono), "%llu", (unsigned long long)deadline);
    cJSON_AddStringToObject(j, "espBoot", esp_hex);
    cJSON_AddStringToObject(j, "deadlineMono", mono);
    char hex[33], session_text[24];
    gw_action_hex_encode(boot, 16, hex);
    cJSON_AddStringToObject(j, "previousBoot", hex);
    uint64_t session;
    do {
        esp_fill_random(&session, sizeof(session));
    } while (!session);
    snprintf(session_text, sizeof(session_text), "%llu", (unsigned long long)session);
    cJSON_AddStringToObject(j, "session", session_text);
    if ((e = gw_ble_pause(deadline)) != ESP_OK)
        goto done;
    if ((e = download(j, deadline)) != ESP_OK)
        goto done;
    if (!strcmp(component, "NRF54")) {
        e = nrf_transfer(j, deadline, details);
        goto done;
    }
    const esp_partition_t *candidate = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, gw_action_string(j, "partition"));
    if (!candidate) {
        e = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if ((e = phase(j, "REBOOT_INTENT")) != ESP_OK)
        goto done;
    if (!task_budget(j, j, &deadline)) {
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    if ((e = esp_ota_set_boot_partition(candidate)) != ESP_OK)
        goto done;
    esp_restart();
    e = ESP_ERR_NOT_FINISHED;
done:
    /* READY, transfer or reboot ambiguity owns maintenance until recovery.
     * A download failure has no active candidate and may release safely. */
    if (e == ESP_OK ||
        (j &&
         (!*gw_action_string(j, "phase") || !strcmp(gw_action_string(j, "phase"), "DOWNLOADING") ||
          !strcmp(gw_action_string(j, "phase"), "VERIFIED_ARTIFACT") ||
          !strcmp(gw_action_string(j, "phase"), "FAILED") ||
          !strcmp(gw_action_string(j, "phase"), "ROLLED_BACK"))) ||
        !j) {
        if (j && e != ESP_OK)
            phase(j, "FAILED");
        gw_core_maintenance_release(id);
        (void)gw_ble_restore(gw_core_mono_ms() + 2000);
    } else
        e = ESP_ERR_NOT_FINISHED;
    cJSON_Delete(j);
    cJSON_Delete(m);
    return e;
}
esp_err_t gw_ota_recover(const cJSON *command_json, uint64_t deadline, cJSON **details) {
    cJSON *j = gw_store_get_json(OTA_KEY);
    const char *id = gw_action_string(command_json, "taskId");
    esp_err_t e = ESP_ERR_TIMEOUT;
    if (!j || strcmp(gw_action_string(j, "taskId"), id)) {
        cJSON_Delete(j);
        gw_core_maintenance_release(id);
        return ESP_ERR_INVALID_STATE;
    }
    if (!strcmp(gw_action_string(j, "phase"), "DOWNLOADING")) {
        phase(j, "FAILED");
        e = ESP_FAIL;
        goto done;
    }
    if (!strcmp(gw_action_string(j, "phase"), "CONFIRMED")) {
        uint8_t boot[16];
        if (!strcmp(gw_action_string(j, "component"), "NRF54") &&
            !gw_action_hex(gw_action_string(j, "confirmedBoot"), boot, 16))
            goto done;
        *details = result(j, !strcmp(gw_action_string(j, "component"), "NRF54") ? boot : NULL);
        e = *details ? ESP_OK : ESP_ERR_NO_MEM;
        goto done;
    }
    if (!strcmp(gw_action_string(j, "phase"), "FAILED") ||
        !strcmp(gw_action_string(j, "phase"), "ROLLED_BACK")) {
        e = ESP_FAIL;
        goto done;
    }
    if ((e = gw_core_maintenance_acquire(id)) != ESP_OK)
        goto done;
    uint64_t authorized_deadline = deadline;
    recovery_authorized = task_budget(command_json, j, &authorized_deadline);
    if (recovery_authorized)
        deadline = authorized_deadline;
    if (!strcmp(gw_action_string(j, "component"), "NRF54")) {
        e = nrf_transfer(j, deadline, details);
        goto done;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (strcmp(running->label, gw_action_string(j, "partition")) ||
        strcmp(esp_app_get_description()->version, gw_action_string(j, "version"))) {
        phase(j, "ROLLED_BACK");
        e = ESP_ERR_INVALID_VERSION;
        goto done;
    }
    uint8_t hash[32], expected[32];
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble < 32 ||
        partition_hash(running, (size_t)size->valuedouble, hash) != ESP_OK ||
        !gw_action_hex(gw_action_string(j, "sha256"), expected, 32) || memcmp(hash, expected, 32)) {
        e = ESP_ERR_INVALID_CRC;
        goto done;
    }
    if (trial_deadline && deadline > trial_deadline - 100)
        deadline = trial_deadline - 100;
    esp_ota_img_states_t running_state;
    if (esp_ota_get_state_partition(running, &running_state) != ESP_OK) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (running_state == ESP_OTA_IMG_PENDING_VERIFY && !recovery_authorized) {
        esp_ota_mark_app_invalid_rollback_and_reboot();
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    if (!gw_core_time_trusted() ||
        (!gw_core_interface_ready("WIFI_STA") && !gw_core_interface_ready("ETHERNET")) ||
        nrf_health(deadline) != ESP_OK) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    uint8_t dfu[62];
    uint32_t caps = 0;
    gw_nrf_identity(NULL, &caps, NULL);
    if ((caps & (1u << 12)) && (dfu_status(dfu, deadline) != ESP_OK || !dfu[9])) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    if ((e = phase(j, "CONFIRMING")) != ESP_OK)
        goto done;
    if (running_state == ESP_OTA_IMG_PENDING_VERIFY && !task_budget(command_json, j, &deadline)) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK)
        goto done;
    if (trial_timer)
        esp_timer_stop(trial_timer);
    trial_deadline = 0;
    e = phase(j, "CONFIRMED");
    if (e == ESP_OK) {
        *details = result(j, NULL);
        if (!*details)
            e = ESP_ERR_NO_MEM;
    }
done:
    if (e == ESP_OK || (j && (!strcmp(gw_action_string(j, "phase"), "ROLLED_BACK") ||
                              !strcmp(gw_action_string(j, "phase"), "FAILED")))) {
        gw_core_maintenance_release(id);
        (void)gw_ble_restore(gw_core_mono_ms() + 2000);
    } else
        e = ESP_ERR_NOT_FINISHED;
    cJSON_Delete(j);
    return e;
}

/* Local uploads share image authentication with cloud OTA, but keep independent
 * durable evidence: no invented cloud task, revision, or UTC requirement. */
#define LOCAL_OTA_KEY "ota/local"
#define LOCAL_OTA_OWNER "local-ota"
#define LOCAL_CHUNK_SIZE 4096
static atomic_bool local_revoked;
void gw_ota_local_revoke(void) { atomic_store(&local_revoked, true); }
static struct {
    cJSON *journal;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    size_t offset, last_offset, last_size;
    uint8_t last_hash[32];
    uint64_t deadline, idle_deadline, boot_started;
    bool boot_ready;
    esp_timer_handle_t reboot_timer;
} local_ota;

static bool local_is(const char *value) {
    return !strcmp(gw_action_string(local_ota.journal, "phase"), value);
}
static bool local_uploading(void) { return local_is("RECEIVING") || local_is("READY"); }
static esp_err_t local_phase(const char *value) {
    cJSON *next = cJSON_Duplicate(local_ota.journal, true);
    if (!next)
        return ESP_ERR_NO_MEM;
    set(next, "phase", cJSON_CreateString(value));
    set(next, "offset", cJSON_CreateNumber((double)local_ota.offset));
    esp_err_t e = gw_store_put_json(LOCAL_OTA_KEY, next);
    if (e == ESP_OK) {
        cJSON_Delete(local_ota.journal);
        local_ota.journal = next;
    } else
        cJSON_Delete(next);
    return e;
}
static void local_release(void) {
    if (local_ota.handle) {
        esp_ota_abort(local_ota.handle);
        local_ota.handle = 0;
    }
    local_ota.deadline = local_ota.idle_deadline = 0;
    gw_core_maintenance_release(LOCAL_OTA_OWNER);
    (void)gw_ble_restore(gw_core_mono_ms() + 2000);
}
static esp_err_t local_stop(const char *state) {
    /* Even a journal write failure must revoke this boot's upload authority. */
    esp_err_t e = local_phase(state);
    local_release();
    if (e != ESP_OK && local_ota.journal)
        set(local_ota.journal, "phase", cJSON_CreateString("FAILED"));
    return e;
}
static cJSON *local_status(void) {
    cJSON *j = local_ota.journal ? cJSON_Duplicate(local_ota.journal, true) : cJSON_CreateObject();
    if (!j)
        return NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(j, "manifestHash");
    cJSON_DeleteItemFromObjectCaseSensitive(j, "partition");
    set(j, "source", cJSON_CreateString("LOCAL"));
    if (!local_ota.journal) {
        set(j, "phase", cJSON_CreateString("NONE"));
        set(j, "sizeBytes", cJSON_CreateNumber(0));
    }
    set(j, "offset", cJSON_CreateNumber((double)local_ota.offset));
    set(j, "chunkSize", cJSON_CreateNumber(LOCAL_CHUNK_SIZE));
    return j;
}
static bool local_keys(const cJSON *j, const char *const *keys, size_t n) {
    if (!cJSON_IsObject(j))
        return false;
    const cJSON *v;
    cJSON_ArrayForEach(v, j) {
        bool found = false;
        for (size_t i = 0; i < n; i++)
            if (v->string && !strcmp(v->string, keys[i]))
                found = true;
        if (!found)
            return false;
    }
    return unique_keys(j, 0);
}
static bool local_uint(const cJSON *j, const char *key, size_t max, size_t *value) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > (double)max || floor(v->valuedouble) != v->valuedouble)
        return false;
    *value = (size_t)v->valuedouble;
    return true;
}
static esp_err_t local_begin(const cJSON *body, uint64_t deadline) {
    static const char *const keys[] = {"uploadId", "manifest", "signature"};
    const char *id = gw_action_string(body, "uploadId"), *raw = gw_action_string(body, "manifest");
    if (!local_keys(body, keys, 3) || !safe_id(id) || strlen(id) > 64 || !*raw ||
        strlen(raw) > 8192 || !manifest_shape(raw))
        return ESP_ERR_INVALID_ARG;
    cJSON *parsed = cJSON_Parse(raw), *command_json = cJSON_CreateObject(), *m = NULL;
    if (!parsed || !command_json) {
        cJSON_Delete(parsed);
        cJSON_Delete(command_json);
        return ESP_ERR_NO_MEM;
    }
    cJSON *params = cJSON_AddObjectToObject(command_json, "parameters");
    cJSON_AddStringToObject(params, "component", "GATEWAY");
    cJSON_AddStringToObject(params, "expectedFirmwareVersion", gw_action_string(parsed, "version"));
    cJSON_Delete(parsed);
    esp_err_t e = verify_manifest(command_json, body, &m);
    cJSON_Delete(command_json);
    if (e != ESP_OK)
        return e;
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    size_t total = 0;
    esp_ota_img_states_t running_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &running_state) == ESP_OK &&
        running_state == ESP_OTA_IMG_PENDING_VERIFY) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    if (!p || p == esp_ota_get_running_partition() ||
        !local_uint(m, "sizeBytes", p->size, &total) || total < 32 ||
        strcmp(gw_action_string(m, "imageFormat"), "ESP_IDF_APP_BIN") ||
        strcmp(gw_action_string(m, "component"), "GATEWAY") ||
        strlen(gw_action_string(m, "version")) >= sizeof(esp_app_get_description()->version)) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
    }
    uint8_t digest[32];
    char hex[65];
    if (mbedtls_sha256((const unsigned char *)raw, strlen(raw), digest, 0)) {
        e = ESP_FAIL;
        goto done;
    }
    gw_action_hex_encode(digest, 32, hex);
    if (!strcmp(id, gw_action_string(local_ota.journal, "uploadId"))) {
        e = !strcmp(hex, gw_action_string(local_ota.journal, "manifestHash"))
                ? ESP_OK
                : ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (local_uploading() || local_is("REBOOT_PENDING") || local_is("CONFIRMING")) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    if (gw_core_mono_ms() >= deadline || atomic_load(&local_revoked)) {
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    if ((e = gw_core_maintenance_acquire(LOCAL_OTA_OWNER)) != ESP_OK) {
        e = ESP_ERR_NOT_FINISHED;
        goto done;
    }
    if ((e = gw_ble_pause(deadline)) != ESP_OK) {
        local_release();
        goto done;
    }
    if (gw_core_mono_ms() >= deadline || atomic_load(&local_revoked)) {
        local_release();
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) {
        local_release();
        e = ESP_ERR_NO_MEM;
        goto done;
    }
    cJSON_AddStringToObject(j, "source", "LOCAL");
    cJSON_AddStringToObject(j, "uploadId", id);
    cJSON_AddStringToObject(j, "version", gw_action_string(m, "version"));
    cJSON_AddStringToObject(j, "sha256", gw_action_string(m, "sha256"));
    cJSON_AddStringToObject(j, "manifestHash", hex);
    cJSON_AddStringToObject(j, "partition", p->label);
    cJSON_AddStringToObject(j, "phase", "RECEIVING");
    cJSON_AddNumberToObject(j, "sizeBytes", (double)total);
    cJSON_AddNumberToObject(j, "offset", 0);
    e = gw_store_put_json(LOCAL_OTA_KEY, j);
    if (e != ESP_OK) {
        cJSON_Delete(j);
        local_release();
        goto done;
    }
    cJSON_Delete(local_ota.journal);
    local_ota.journal = j;
    local_ota.partition = p;
    local_ota.offset = local_ota.last_size = local_ota.last_offset = 0;
    local_ota.deadline = deadline;
    local_ota.idle_deadline = gw_core_mono_ms() + 300000;
    if (gw_core_mono_ms() >= deadline || atomic_load(&local_revoked)) {
        (void)local_stop("ABORTED");
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    e = esp_ota_begin(p, total, &local_ota.handle);
    if (e == ESP_OK && (gw_core_mono_ms() >= deadline || atomic_load(&local_revoked)))
        e = ESP_ERR_TIMEOUT;
    if (e != ESP_OK)
        (void)local_stop("FAILED");
done:
    cJSON_Delete(m);
    return e;
}
static esp_err_t local_write(const cJSON *body) {
    static const char *const keys[] = {"uploadId", "offset", "data"};
    size_t offset, n = 0, total = 0;
    const char *encoded = gw_action_string(body, "data");
    size_t text_size = strlen(encoded);
    if (!local_keys(body, keys, 3) || !local_ota.partition ||
        !local_uint(body, "offset", local_ota.partition->size, &offset) || !text_size ||
        text_size > 5464 || text_size % 4 ||
        !local_uint(local_ota.journal, "sizeBytes", local_ota.partition->size, &total))
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < text_size; i++) {
        unsigned char c = (unsigned char)encoded[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '/' ||
              (c == '=' && i >= text_size - 2 && (i == text_size - 1 || encoded[i + 1] == '='))))
            return ESP_ERR_INVALID_ARG;
    }
    uint8_t *data = malloc(LOCAL_CHUNK_SIZE), digest[32];
    if (!data)
        return ESP_ERR_NO_MEM;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (mbedtls_base64_decode(data, LOCAL_CHUNK_SIZE, &n, (const unsigned char *)encoded,
                              text_size) ||
        !n || n > LOCAL_CHUNK_SIZE || mbedtls_sha256(data, n, digest, 0))
        goto done;
    if (local_uploading() &&
        (gw_core_mono_ms() >= local_ota.deadline || atomic_load(&local_revoked))) {
        (void)local_stop("ABORTED");
        e = ESP_ERR_TIMEOUT;
        goto done;
    }
    if (local_uploading() && offset == local_ota.last_offset && local_ota.last_size == n &&
        !memcmp(digest, local_ota.last_hash, 32)) {
        e = ESP_OK;
        goto done;
    }
    if (!local_is("RECEIVING") || !local_ota.handle || offset != local_ota.offset ||
        n > total - offset) {
        e = ESP_ERR_INVALID_STATE;
        goto done;
    }
    e = esp_ota_write(local_ota.handle, data, n);
    if (e == ESP_OK && (gw_core_mono_ms() >= local_ota.deadline || atomic_load(&local_revoked)))
        e = ESP_ERR_TIMEOUT;
    if (e != ESP_OK) {
        (void)local_stop("FAILED");
        goto done;
    }
    local_ota.last_offset = offset;
    local_ota.last_size = n;
    memcpy(local_ota.last_hash, digest, 32);
    local_ota.offset += n;
    if (local_ota.offset == total) {
        e = esp_ota_end(local_ota.handle);
        local_ota.handle = 0;
        esp_app_desc_t desc;
        uint8_t expected[32];
        if (e == ESP_OK &&
            (partition_hash(local_ota.partition, total, digest) != ESP_OK ||
             !gw_action_hex(gw_action_string(local_ota.journal, "sha256"), expected, 32) ||
             memcmp(digest, expected, 32)))
            e = ESP_ERR_INVALID_CRC;
        if (e == ESP_OK &&
            (esp_ota_get_partition_description(local_ota.partition, &desc) != ESP_OK ||
             !memchr(desc.version, 0, sizeof(desc.version)) ||
             strcmp(desc.version, gw_action_string(local_ota.journal, "version"))))
            e = ESP_ERR_INVALID_VERSION;
        if (e == ESP_OK && (gw_core_mono_ms() >= local_ota.deadline || atomic_load(&local_revoked)))
            e = ESP_ERR_TIMEOUT;
        if (e == ESP_OK)
            e = local_phase("READY");
        if (e != ESP_OK)
            (void)local_stop("FAILED");
    }
done:
    free(data);
    return e;
}
static void local_restart(void *unused) {
    (void)unused;
    esp_restart();
}
static esp_err_t local_commit(void) {
    if (local_is("REBOOT_PENDING"))
        return ESP_OK;
    if (!local_is("READY") || !local_ota.partition)
        return ESP_ERR_INVALID_STATE;
    esp_err_t e;
    if (!local_ota.reboot_timer) {
        const esp_timer_create_args_t args = {.callback = local_restart, .name = "local_reboot"};
        if ((e = esp_timer_create(&args, &local_ota.reboot_timer)) != ESP_OK)
            return e;
    }
    if ((e = local_phase("REBOOT_PENDING")) != ESP_OK)
        return e;
    if (gw_core_mono_ms() >= local_ota.deadline || atomic_load(&local_revoked)) {
        (void)local_stop("ABORTED");
        return ESP_ERR_TIMEOUT;
    }
    e = esp_ota_set_boot_partition(local_ota.partition);
    /* A metadata write can take effect before returning an I/O error. Retain
     * the durable intent and lease even then; the next boot determines the
     * actual slot and either confirms it or records rollback. */
    esp_err_t scheduled = esp_timer_start_once(local_ota.reboot_timer, 2000000);
    if (scheduled != ESP_OK)
        esp_restart();
    return e == ESP_OK ? ESP_OK : ESP_FAIL;
}
void gw_ota_local_window_closed(void) {
    atomic_store(&local_revoked, false);
    if (local_uploading())
        (void)local_stop("ABORTED");
}
void gw_ota_local_boot_ready(void) { local_ota.boot_ready = true; }
esp_err_t gw_ota_local_init(void) {
    cJSON_Delete(local_ota.journal);
    local_ota.journal = gw_store_get_json(LOCAL_OTA_KEY);
    local_ota.boot_started = gw_core_mono_ms();
    local_ota.boot_ready = false;
    local_ota.handle = 0;
    local_ota.offset = 0;
    if (!local_ota.journal)
        return ESP_OK;
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(local_ota.journal, "offset");
    if (cJSON_IsNumber(offset) && offset->valuedouble >= 0 && offset->valuedouble <= 0x280000)
        local_ota.offset = (size_t)offset->valuedouble;
    if (local_uploading())
        return local_stop("ABORTED");
    if (local_is("REBOOT_PENDING") || local_is("CONFIRMING")) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (!running || strcmp(running->label, gw_action_string(local_ota.journal, "partition")) ||
            strcmp(esp_app_get_description()->version,
                   gw_action_string(local_ota.journal, "version")))
            return local_stop("ROLLED_BACK");
        return gw_core_maintenance_acquire(LOCAL_OTA_OWNER);
    }
    return ESP_OK;
}
void gw_ota_local_tick(void) {
    uint64_t now = gw_core_mono_ms();
    if (local_uploading() &&
        (!local_ota.deadline || now >= local_ota.deadline || now >= local_ota.idle_deadline)) {
        (void)local_stop("ABORTED");
        return;
    }
    if (!(local_is("REBOOT_PENDING") || local_is("CONFIRMING")) || !local_ota.boot_ready ||
        now < local_ota.boot_started + 10000)
        return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    /* REBOOT_PENDING can still be observed on the uploading boot. */
    if (!running || strcmp(running->label, gw_action_string(local_ota.journal, "partition")) ||
        strcmp(esp_app_get_description()->version, gw_action_string(local_ota.journal, "version")))
        return;
    uint64_t deadline = now + 2500;
    if (trial_deadline && deadline >= trial_deadline)
        return;
    size_t total;
    uint8_t hash[32], expected[32];
    if (!local_uint(local_ota.journal, "sizeBytes", running->size, &total) ||
        partition_hash(running, total, hash) != ESP_OK ||
        !gw_action_hex(gw_action_string(local_ota.journal, "sha256"), expected, 32) ||
        memcmp(hash, expected, 32) || nrf_health(deadline) != ESP_OK)
        return;
    uint32_t caps = 0;
    uint8_t dfu[62];
    if (!gw_nrf_identity(NULL, &caps, NULL) ||
        ((caps & (1u << 12)) && (dfu_status(dfu, deadline) != ESP_OK || !dfu[9])))
        return;
    if (local_phase("CONFIRMING") != ESP_OK)
        return;
    if ((trial_deadline && gw_core_mono_ms() + 100 >= trial_deadline) ||
        esp_ota_mark_app_valid_cancel_rollback() != ESP_OK)
        return;
    if (trial_timer)
        esp_timer_stop(trial_timer);
    trial_deadline = 0;
    if (local_phase("CONFIRMED") == ESP_OK)
        local_release();
}
esp_err_t gw_ota_local_dispatch(const cJSON *request, uint64_t window_deadline, cJSON **response) {
    static const char *const envelope[] = {
        "apiVersion",       "requestId", "operation", "body", "expectedNetworkGeneration",
        "baseLocalRevision"};
    static const char *const id_only[] = {"uploadId"};
    if (!response)
        return ESP_ERR_INVALID_ARG;
    *response = NULL;
    const char *id = gw_action_string(request, "requestId"),
               *op = gw_action_string(request, "operation");
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(request, "body");
    const char *code = "OK";
    esp_err_t e = ESP_OK;
    gw_ota_local_tick();
    uint64_t now = gw_core_mono_ms();
    if (!local_keys(request, envelope, 6) || !safe_id(id) || strlen(id) > 64 ||
        strcmp(gw_action_string(request, "apiVersion"), "1.0") || !cJSON_IsObject(body))
        code = "INVALID_ARGUMENT";
    else if (window_deadline <= now)
        code = "SESSION_EXPIRED";
    else if (!strcmp(op, "OTA_BEGIN"))
        e = local_begin(body, window_deadline);
    else if (!strcmp(op, "OTA_STATUS")) {
        if (!local_keys(body, id_only, 1))
            e = ESP_ERR_INVALID_ARG;
        else if (*gw_action_string(body, "uploadId") &&
                 strcmp(gw_action_string(body, "uploadId"),
                        gw_action_string(local_ota.journal, "uploadId")))
            e = ESP_ERR_INVALID_STATE;
    } else if (!*gw_action_string(body, "uploadId") ||
               strcmp(gw_action_string(body, "uploadId"),
                      gw_action_string(local_ota.journal, "uploadId")))
        e = ESP_ERR_INVALID_STATE;
    else if (!strcmp(op, "OTA_WRITE"))
        e = local_write(body);
    else if (!strcmp(op, "OTA_COMMIT") && local_keys(body, id_only, 1))
        e = local_commit();
    else if (!strcmp(op, "OTA_ABORT") && local_keys(body, id_only, 1)) {
        if (local_uploading())
            e = local_stop("ABORTED");
        else if (!local_is("ABORTED"))
            e = ESP_ERR_INVALID_STATE;
    } else
        e = ESP_ERR_INVALID_ARG;
    if (e != ESP_OK)
        code =
            e == ESP_ERR_INVALID_ARG || e == ESP_ERR_INVALID_SIZE || e == ESP_ERR_INVALID_RESPONSE
                ? "INVALID_ARGUMENT"
            : e == ESP_ERR_INVALID_CRC     ? "INTEGRITY_ERROR"
            : e == ESP_ERR_INVALID_VERSION ? "VERSION_MISMATCH"
            : e == ESP_ERR_TIMEOUT         ? "SESSION_EXPIRED"
            : e == ESP_ERR_NOT_FINISHED    ? "BUSY"
            : e == ESP_ERR_INVALID_STATE   ? "CONFLICT"
                                           : "STORAGE_FAULT";
    else if (!strcmp(code, "OK") && !strcmp(op, "OTA_COMMIT"))
        code = "ACCEPTED";
    if (e == ESP_OK && !strcmp(code, "OK") && local_uploading())
        local_ota.idle_deadline = now + 300000;
    cJSON *out = cJSON_CreateObject(), *status = local_status();
    if (!out || !status) {
        cJSON_Delete(out);
        cJSON_Delete(status);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(out, "apiVersion", "1.0");
    cJSON_AddStringToObject(out, "requestId", id);
    cJSON_AddStringToObject(out, "code", code);
    cJSON_AddItemToObject(out, "body", status);
    *response = out;
    return ESP_OK;
}
