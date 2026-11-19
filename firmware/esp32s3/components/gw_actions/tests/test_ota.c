/* Actual OTA executor with deterministic Flash/UART/HTTP/clock boundaries.
 * SHA boundary uses test SHA512 truncation; Ed25519 uses production Monocypher.
 * Cryptographic cross-implementation validation lives in test_ed25519.py. */
#include "../gw_ota.c"
#include <assert.h>
static cJSON *database, *verification;
static uint8_t image[1024], flash[4096], nrf_flash[4096], peer_boot[16] = {1};
static uint64_t monotonic = 1000, dfu_session;
static size_t written, dfu_offset, dfu_size;
static uint8_t dfu_hash[32], dfu_state;
static bool bad_stream, fail_store, untrusted, bad_signature, trial, trial_nrf, confirmed,
    fail_confirm;
static unsigned restarts, boots, confirms, aborts, uart_chunks, begin_count;
static char owner[129];
static esp_partition_t slot0 = {.size = 4096, .label = "ota_0"},
                       slot1 = {.size = 4096, .label = "ota_1"},
                       cache = {.size = 4096, .label = "nrf_image"};
static esp_app_desc_t description = {.version = "v1"};
static const esp_partition_t *running = &slot0;
static char public_base64[64];
const char *gw_action_string(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
void gw_action_hex_encode(const uint8_t *p, size_t n, char *s) {
    for (size_t i = 0; i < n; i++)
        sprintf(s + 2 * i, "%02x", p[i]);
    s[2 * n] = 0;
}
bool gw_action_hex(const char *s, uint8_t *p, size_t n) {
    if (strlen(s) != 2 * n)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return false;
        p[i] = (uint8_t)v;
    }
    return true;
}
uint64_t gw_core_mono_ms(void) { return monotonic; }
bool gw_core_time_trusted(void) { return !untrusted; }
bool gw_core_interface_ready(const char *s) {
    (void)s;
    return true;
}
void gw_capture_esp_boot(uint8_t b[16]) { memset(b, trial ? 7 : 6, 16); }
void vTaskDelay(TickType_t t) { monotonic += t; }
cJSON *gw_store_get_json(const char *key) {
    return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(database, key), true);
}
esp_err_t gw_store_put_json(const char *key, const cJSON *v) {
    if (fail_store)
        return ESP_FAIL;
    cJSON_DeleteItemFromObjectCaseSensitive(database, key);
    cJSON_AddItemToObject(database, key, cJSON_Duplicate(v, true));
    return ESP_OK;
}
esp_err_t gw_core_maintenance_acquire(const char *s) {
    if (*owner && strcmp(owner, s))
        return ESP_ERR_INVALID_STATE;
    snprintf(owner, sizeof(owner), "%s", s);
    return ESP_OK;
}
void gw_core_maintenance_release(const char *s) {
    if (!strcmp(owner, s))
        *owner = 0;
}
esp_err_t gw_factory_get_string(const char *key, char *out, size_t cap) {
    if (!strcmp(key, "artifact_keys")) {
        snprintf(out, cap, "{\"release-test\":\"%s\"}", public_base64);
        return ESP_OK;
    }
    snprintf(out, cap, "%s", !strcmp(key, "model") ? "GW32I33V2" : "R2");
    return ESP_OK;
}
void esp_fill_random(void *p, size_t n) { memset(p, 5, n); }
void esp_restart(void) { restarts++; }
const esp_app_desc_t *esp_app_get_description(void) { return &description; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {
    (void)p;
    return &slot1;
}
const esp_partition_t *esp_ota_get_running_partition(void) { return running; }
const esp_partition_t *esp_partition_find_first(int type, int sub, const char *label) {
    (void)type;
    (void)sub;
    return !strcmp(label, "nrf_image") ? &cache : !strcmp(label, "ota_1") ? &slot1 : NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t offset, void *out, size_t n) {
    if (offset + n > p->size)
        return ESP_FAIL;
    memcpy(out, (p == &cache ? nrf_flash : flash) + offset, n);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t offset, const void *in, size_t n) {
    assert(offset % 16 == 0 && n % 16 == 0);
    if (offset + n > p->size)
        return ESP_FAIL;
    memcpy((p == &cache ? nrf_flash : flash) + offset, in, n);
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t n) {
    (void)offset;
    memset(p == &cache ? nrf_flash : flash, 255, n);
    return ESP_OK;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h) {
    (void)p;
    (void)n;
    *h = 1;
    written = 0;
    begin_count++;
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *p, size_t n) {
    (void)h;
    memcpy(flash + written, p, n);
    written += n;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) {
    (void)h;
    return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h) {
    (void)h;
    aborts++;
    return ESP_OK;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d) {
    (void)p;
    strcpy(d->version, "v2");
    return ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {
    assert(p == &slot1);
    boots++;
    return ESP_OK;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) {
    (void)p;
    *s = trial && !confirmed ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {
    confirms++;
    confirmed = true;
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) {
    aborts++;
    return ESP_OK;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
    assert(args->callback);
    *timer = (void *)1;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    (void)t;
    assert(us == 90000000);
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    (void)t;
    return ESP_OK;
}
esp_err_t gw_ble_pause(uint64_t d) {
    (void)d;
    return ESP_OK;
}
esp_err_t gw_ble_restore(uint64_t d) {
    (void)d;
    return ESP_OK;
}
bool gw_nrf_identity(uint8_t b[16], uint32_t *c, char v[64]) {
    if (b)
        memcpy(b, peer_boot, 16);
    if (c)
        *c = 1u << 12;
    if (v)
        strcpy(v, trial_nrf ? "v2" : "v1");
    return true;
}
esp_err_t gw_nrf_rehandshake(int64_t d) {
    (void)d;
    return ESP_OK;
}
static void status(uint8_t *s) {
    memset(s, 0, 62);
    gw_put_u64(s, dfu_session);
    s[8] = dfu_state;
    s[9] = !trial_nrf || confirmed;
    gw_put_u32(s + 10, (uint32_t)dfu_offset);
    gw_put_u32(s + 14, (uint32_t)dfu_size);
    gw_put_u32(s + 18, 4096);
    memcpy(s + 26, dfu_hash, 32);
    gw_put_u32(s + 58, 120000);
}
esp_err_t gw_nrf_command(uint8_t opcode, const void *arg, size_t length, uint8_t *result, void *out,
                         size_t *n, int64_t deadline) {
    (void)deadline;
    const uint8_t *a = arg;
    *result = 0;
    switch (opcode) {
    case 3:
        assert(*n >= 71);
        memset(out, 0, 71);
        *n = 71;
        return ESP_OK;
    case 0x30:
        assert(length == 44);
        dfu_session = gw_get_u64(a);
        dfu_size = gw_get_u32(a + 8);
        memcpy(dfu_hash, a + 12, 32);
        dfu_offset = 0;
        dfu_state = 1;
        break;
    case 0x31:
        assert(gw_get_u32(a + 8) == dfu_offset);
        dfu_offset += gw_get_u16(a + 12);
        uart_chunks++;
        break;
    case 0x32:
        assert(dfu_offset == dfu_size);
        dfu_state = 2;
        break;
    case 0x33:
        break;
    case 0x34:
        dfu_state = 4;
        aborts++;
        break;
    case 0x35:
        peer_boot[0]++;
        dfu_state = 0;
        trial_nrf = true;
        *n = 0;
        return ESP_OK;
    case 0x37:
        assert(length == 36 && gw_get_u32(a) == sizeof(image));
        *n = 0;
        return ESP_OK;
    case 0x36:
        if (fail_confirm)
            return ESP_ERR_TIMEOUT;
        confirmed = true;
        confirms++;
        *n = 0;
        return ESP_OK;
    default:
        assert(0);
    }
    assert(*n >= 62);
    status(out);
    *n = 62;
    return ESP_OK;
}
int mbedtls_sha256(const unsigned char *p, size_t n, unsigned char out[32], int mode) {
    (void)mode;
    uint8_t full[64];
    crypto_sha512(full, p, n);
    memcpy(out, full, 32);
    return 0;
}
void mbedtls_sha256_init(mbedtls_sha256_context *c) { memset(c, 0, sizeof(*c)); }
void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }
int mbedtls_sha256_starts(mbedtls_sha256_context *c, int mode) {
    (void)mode;
    crypto_sha512_init(&c->ctx);
    return 0;
}
int mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *p, size_t n) {
    crypto_sha512_update(&c->ctx, p, n);
    return 0;
}
int mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32]) {
    uint8_t full[64];
    crypto_sha512_final(&c->ctx, full);
    memcpy(out, full, 32);
    return 0;
}
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *n, const unsigned char *in,
                          size_t size) {
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((size + 2) / 3) * 4;
    if (cap < need + 1)
        return -1;
    size_t j = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < size)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < size)
            v |= in[i + 2];
        out[j++] = b[v >> 18];
        out[j++] = b[(v >> 12) & 63];
        out[j++] = i + 1 < size ? b[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < size ? b[v & 63] : '=';
    }
    out[j] = 0;
    *n = j;
    return 0;
}
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n, const unsigned char *in,
                          size_t length) {
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned value = 0, bits = 0;
    *n = 0;
    for (size_t i = 0; i < length; i++) {
        if (in[i] == '=')
            break;
        const char *p = strchr(b, in[i]);
        if (!p)
            return -1;
        value = (value << 6) | (unsigned)(p - b);
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
esp_err_t gw_http_json(const char *path, const char *method, const cJSON *body,
                       const char *interface, uint64_t deadline, cJSON **out) {
    (void)path;
    (void)method;
    (void)body;
    (void)interface;
    (void)deadline;
    *out = cJSON_Duplicate(verification, true);
    if (bad_signature)
        cJSON_ReplaceItemInObjectCaseSensitive(*out, "signature", cJSON_CreateString("bad"));
    return ESP_OK;
}
esp_err_t gw_http_stream(const char *path, uint64_t deadline, size_t limit, gw_http_sink_fn sink,
                         void *context, size_t *received) {
    (void)path;
    (void)deadline;
    (void)limit;
    uint8_t copy[sizeof(image)];
    memcpy(copy, image, sizeof(copy));
    if (bad_stream)
        copy[1] ^= 1;
    *received = sizeof(copy);
    return sink(copy, sizeof(copy), context);
}
static cJSON *setup(bool nrf) {
    cJSON_Delete(database);
    database = cJSON_CreateObject();
    cJSON_Delete(verification);
    verification = NULL;
    monotonic = 1000;
    memset(peer_boot, 1, 16);
    *owner = 0;
    bad_stream = fail_store = untrusted = bad_signature = trial = trial_nrf = confirmed =
        fail_confirm = false;
    restarts = boots = confirms = aborts = uart_chunks = begin_count = 0;
    dfu_state = 0;
    dfu_session = 0;
    running = &slot0;
    strcpy(description.version, "v1");
    trial_deadline = 0;
    trial_timer = NULL;
    for (size_t i = 0; i < sizeof(image); i++)
        image[i] = (uint8_t)i;
    uint8_t digest[32], seed[32] = {9}, secret[64], public[32], signature[64];
    char hex[65], sig[89];
    size_t n;
    mbedtls_sha256(image, sizeof(image), digest, 0);
    gw_action_hex_encode(digest, 32, hex);
    crypto_ed25519_key_pair(secret, public, seed);
    mbedtls_base64_encode((unsigned char *)public_base64, sizeof(public_base64), &n, public, 32);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "keyId", "release-test");
    cJSON_AddStringToObject(m, "name", "test");
    cJSON_AddStringToObject(m, "kind", "FIRMWARE");
    cJSON_AddStringToObject(m, "version", "v2");
    cJSON_AddStringToObject(m, "component", nrf ? "NRF54" : "GATEWAY");
    cJSON_AddStringToObject(m, "sha256", hex);
    cJSON_AddStringToObject(m, "imageFormat", "MCUBOOT_SIGNED_BIN");
    cJSON *models = cJSON_AddArrayToObject(m, "compatibleModels");
    cJSON_AddItemToArray(models, cJSON_CreateString("GW32I33V2"));
    char *raw = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    crypto_ed25519_sign(signature, secret, (const uint8_t *)raw, strlen(raw));
    mbedtls_base64_encode((unsigned char *)sig, sizeof(sig), &n, signature, 64);
    verification = cJSON_CreateObject();
    cJSON_AddStringToObject(verification, "manifest", raw);
    cJSON_AddStringToObject(verification, "signature", sig);
    free(raw);
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "taskId", "upgrade-test");
    cJSON_AddStringToObject(c, "expiresAt", "2099-01-01T00:00:00Z");
    cJSON *p = cJSON_AddObjectToObject(c, "parameters");
    cJSON_AddStringToObject(p, "component", nrf ? "NRF54" : "GATEWAY");
    cJSON_AddStringToObject(p, "artifactId", "artifact-test");
    cJSON_AddStringToObject(p, "expectedFirmwareVersion", "v2");
    return c;
}
int main(void) {
    assert(expires_seconds("2024-02-29T12:00:00Z") > 0);
    assert(expires_seconds("2023-02-29T12:00:00Z") < 0);
    assert(expires_seconds("2024-01-01T00:00:00+08:00") == expires_seconds("2023-12-31T16:00:00Z"));
    cJSON *details = NULL, *c = setup(false);
    bad_signature = true;
    assert(gw_ota_execute(c, 100000, &details) != ESP_OK && begin_count == 0 && boots == 0);
    cJSON_Delete(c);
    c = setup(false);
    fail_store = true;
    assert(gw_ota_execute(c, 100000, &details) != ESP_OK && begin_count == 0);
    cJSON_Delete(c);
    c = setup(false);
    bad_stream = true;
    assert(gw_ota_execute(c, 100000, &details) == ESP_ERR_INVALID_CRC && boots == 0 && aborts == 1);
    cJSON_Delete(c);
    c = setup(false);
    assert(gw_ota_execute(c, 100000, &details) == ESP_ERR_NOT_FINISHED && boots == 1 &&
           restarts == 1 && confirms == 0);
    trial = true;
    running = &slot1;
    strcpy(description.version, "v2");
    assert(gw_ota_init() == ESP_OK);
    assert(gw_ota_recover(c, 100000, &details) == ESP_OK && confirms == 1 && !*owner);
    cJSON_Delete(details);
    cJSON_Delete(c);
    c = setup(false);
    assert(gw_ota_execute(c, 100000, &details) == ESP_ERR_NOT_FINISHED);
    trial = true;
    running = &slot1;
    strcpy(description.version, "v2");
    cJSON_ReplaceItemInObjectCaseSensitive(c, "expiresAt",
                                           cJSON_CreateString("2020-01-01T00:00:00Z"));
    assert(gw_ota_recover(c, 100000, &details) == ESP_ERR_NOT_FINISHED && confirms == 0 &&
           aborts == 1);
    cJSON_Delete(c);
    c = setup(true);
    assert(gw_ota_execute(c, 100000, &details) == ESP_OK && uart_chunks == 1 && confirms == 1 &&
           !*owner);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(details, "bootConfirmed")));
    cJSON_Delete(details);
    unsigned before = uart_chunks;
    assert(gw_ota_recover(c, 100000, &details) == ESP_OK && uart_chunks == before);
    cJSON_Delete(details);
    cJSON_Delete(c);
    c = setup(true);
    fail_confirm = true;
    assert(gw_ota_execute(c, 100000, &details) == ESP_ERR_NOT_FINISHED && confirms == 0 && *owner);
    before = uart_chunks;
    fail_confirm = false;
    assert(gw_ota_recover(c, 100000, &details) == ESP_OK && confirms == 1 && uart_chunks == before);
    cJSON_Delete(details);
    cJSON_Delete(c);
    cJSON_Delete(database);
    cJSON_Delete(verification);
    puts("PASS: OTA manifest/store/hash barriers, ESP trial/expired rollback, nRF "
         "streaming/confirmation recovery");
    return 0;
}
