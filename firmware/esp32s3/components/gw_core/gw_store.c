#include "esp_flash_encrypt.h"
#include "esp_partition.h"
#include "gw_internal.h"
#include "gw_rules.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include <string.h>

#define STORE_MAX (96u * 1024u)
#define STORE_MAX_KEYS 256
static SemaphoreHandle_t store_lock;
static nvs_handle_t store_handle, factory_handle, secret_handle;
static cJSON *store_root;
static uint64_t store_generation;
static unsigned store_slot;
static bool store_fault;

#define crc32 gw_record_crc32
#define put32 gw_record_put32
#define get32 gw_record_get32
#define put64 gw_record_put64
#define get64 gw_record_get64

static esp_err_t persist(cJSON *next) {
  char *json = cJSON_PrintUnformatted(next);
  if (!json)
    return ESP_ERR_NO_MEM;
  size_t len = strlen(json);
  if (len > STORE_MAX || store_generation == UINT64_MAX) {
    free(json);
    return ESP_ERR_NO_MEM;
  }
  uint8_t *record = malloc(len + 24);
  if (!record) {
    free(json);
    return ESP_ERR_NO_MEM;
  }
  memcpy(record, "GWSS", 4);
  put32(record + 4, (uint32_t)len);
  put64(record + 8, store_generation + 1);
  put32(record + 16, crc32(json, len));
  put32(record + 20, crc32(record, 20));
  memcpy(record + 24, json, len);
  free(json);
  unsigned slot = store_slot ^ 1u;
  const char *key = slot ? "snapshot1" : "snapshot0";
  esp_err_t err = nvs_set_blob(store_handle, key, record, len + 24);
  if (err == ESP_OK)
    err = nvs_commit(store_handle);
  uint8_t *readback = NULL;
  if (err == ESP_OK) {
    readback = malloc(len + 24);
    size_t got = len + 24;
    if (!readback)
      err = ESP_ERR_NO_MEM;
    else {
      err = nvs_get_blob(store_handle, key, readback, &got);
      if (err == ESP_OK && (got != len + 24 || memcmp(record, readback, got)))
        err = ESP_FAIL;
    }
  }
  free(readback);
  if (err == ESP_OK) {
    uint8_t pointer[24] = {0};
    memcpy(pointer, "GWSA", 4);
    put32(pointer + 4, slot);
    put64(pointer + 8, store_generation + 1);
    put32(pointer + 16, get32(record + 16));
    put32(pointer + 20, crc32(pointer, 20));
    err = nvs_set_blob(store_handle, "active", pointer, sizeof(pointer));
    if (err == ESP_OK)
      err = nvs_commit(store_handle);
    if (err == ESP_OK) {
      uint8_t verify[24];
      size_t n = sizeof(verify);
      err = nvs_get_blob(store_handle, "active", verify, &n);
      if (err == ESP_OK && (n != sizeof(verify) || memcmp(pointer, verify, n)))
        err = ESP_FAIL;
    }
  }
  free(record);
  if (err == ESP_OK) {
    cJSON_Delete(store_root);
    store_root = next;
    store_generation++;
    store_slot = slot;
  } else
    store_fault = true; /* Never continue after an ambiguous commit. */
  return err;
}

esp_err_t gw_store_init(void) {
  if (store_lock)
    return ESP_ERR_INVALID_STATE;
#if CONFIG_GW_REQUIRE_FLASH_ENCRYPTION
  if (!esp_flash_encryption_enabled())
    return ESP_ERR_INVALID_STATE;
#endif
  const esp_partition_t *keys = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, "nvs_keys");
  if (!keys)
    return ESP_ERR_NOT_FOUND;
  nvs_sec_cfg_t security = {0};
  esp_err_t err = nvs_flash_read_security_cfg(keys, &security);
  if (err != ESP_OK)
    return err; /* Manufacturing owns key generation. */
  err = nvs_flash_secure_init_partition("gwfactory", &security);
  if (err == ESP_OK)
    err = nvs_flash_secure_init_partition("gwstore", &security);
#if CONFIG_BT_NIMBLE_NVS_PERSIST
  /* NimBLE uses nvs_open(), hence the default named partition. Initialize it
   * explicitly with the same manufacturing-provisioned NVS key; never call
   * nvs_flash_init() or generate/erase keys on a restore failure. */
  if (err == ESP_OK)
    err = nvs_flash_secure_init_partition("nvs", &security);
#endif
  memset(&security, 0, sizeof(security));
  if (err != ESP_OK)
    return err; /* No automatic erase of credentials/journal. */
  err = nvs_open_from_partition("gwfactory", "factory", NVS_READONLY,
                                &factory_handle);
  if (err == ESP_OK)
    err = nvs_open_from_partition("gwstore", "gateway", NVS_READWRITE,
                                  &store_handle);
  if (err == ESP_OK)
    err = nvs_open_from_partition("gwstore", "secrets", NVS_READWRITE,
                                  &secret_handle);
  if (err != ESP_OK)
    return err;
  store_lock = xSemaphoreCreateMutex();
  if (!store_lock)
    return ESP_ERR_NO_MEM;
  uint8_t pointer[24];
  size_t size = sizeof(pointer);
  err = nvs_get_blob(store_handle, "active", pointer, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    size_t ignored = 0;
    if (nvs_get_blob(store_handle, "snapshot0", NULL, &ignored) !=
            ESP_ERR_NVS_NOT_FOUND ||
        nvs_get_blob(store_handle, "snapshot1", NULL, &ignored) !=
            ESP_ERR_NVS_NOT_FOUND)
      return ESP_ERR_INVALID_STATE;
    store_root = cJSON_CreateObject();
    return store_root ? ESP_OK : ESP_ERR_NO_MEM;
  }
  if (err != ESP_OK || !gw_record_valid_pointer(pointer, size))
    return ESP_ERR_INVALID_STATE;
  store_slot = get32(pointer + 4);
  store_generation = get64(pointer + 8);
  const char *key = store_slot ? "snapshot1" : "snapshot0";
  size = 0;
  err = nvs_get_blob(store_handle, key, NULL, &size);
  if (err != ESP_OK || size < 24 || size > STORE_MAX + 24)
    return ESP_ERR_INVALID_STATE;
  uint8_t *record = malloc(size + 1);
  if (!record)
    return ESP_ERR_NO_MEM;
  size_t got = size;
  err = nvs_get_blob(store_handle, key, record, &got);
  if (err == ESP_OK &&
      (got != size || !gw_record_valid_snapshot(record, size, store_generation,
                                                get32(pointer + 16))))
    err = ESP_ERR_INVALID_STATE;
  if (err == ESP_OK) {
    record[size] = 0;
    store_root = cJSON_Parse((const char *)record + 24);
    if (!cJSON_IsObject(store_root))
      err = ESP_ERR_INVALID_STATE;
  }
  free(record);
  store_fault = err != ESP_OK;
  return err;
}

esp_err_t gw_store_transaction(const gw_store_op_t *ops, size_t count) {
  if (!store_root || !ops || !count || count > 64)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(store_lock, portMAX_DELAY);
  if (store_fault) {
    xSemaphoreGive(store_lock);
    return ESP_ERR_INVALID_STATE;
  }
  cJSON *next = cJSON_Duplicate(store_root, true);
  esp_err_t err = next ? ESP_OK : ESP_ERR_NO_MEM;
  for (size_t i = 0; i < count && err == ESP_OK; i++) {
    if (!ops[i].key || !*ops[i].key || strlen(ops[i].key) > 255 ||
        ops[i].size > STORE_MAX ||
        (!ops[i].remove && !ops[i].data && ops[i].size)) {
      err = ESP_ERR_INVALID_ARG;
      break;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(next, ops[i].key);
    if (!ops[i].remove) {
      size_t cap = ((ops[i].size + 2) / 3) * 4 + 1, written = 0;
      unsigned char *b64 = malloc(cap);
      if (!b64) {
        err = ESP_ERR_NO_MEM;
        break;
      }
      int rc =
          mbedtls_base64_encode(b64, cap, &written, ops[i].data, ops[i].size);
      if (rc) {
        free(b64);
        err = ESP_FAIL;
        break;
      }
      b64[written] = 0;
      if (!cJSON_AddStringToObject(next, ops[i].key, (const char *)b64))
        err = ESP_ERR_NO_MEM;
      free(b64);
    }
  }
  if (err == ESP_OK && cJSON_GetArraySize(next) > STORE_MAX_KEYS)
    err = ESP_ERR_NO_MEM;
  if (err == ESP_OK)
    err = persist(next);
  if (err != ESP_OK)
    cJSON_Delete(next);
  xSemaphoreGive(store_lock);
  return err;
}
esp_err_t gw_store_get(const char *key, void *data, size_t *size) {
  if (!store_root || !key || !size)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(store_lock, portMAX_DELAY);
  cJSON *value = cJSON_GetObjectItemCaseSensitive(store_root, key);
  esp_err_t err = ESP_ERR_NOT_FOUND;
  if (cJSON_IsString(value)) {
    size_t needed = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &needed,
                                   (const unsigned char *)value->valuestring,
                                   strlen(value->valuestring));
    if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
      err = ESP_FAIL;
    else if (!data) {
      *size = needed;
      err = ESP_OK;
    } else if (*size < needed) {
      *size = needed;
      err = ESP_ERR_INVALID_SIZE;
    } else {
      rc = mbedtls_base64_decode(data, *size, &needed,
                                 (const unsigned char *)value->valuestring,
                                 strlen(value->valuestring));
      *size = needed;
      err = rc ? ESP_FAIL : ESP_OK;
    }
  }
  xSemaphoreGive(store_lock);
  return err;
}
esp_err_t gw_store_put(const char *key, const void *data, size_t size) {
  gw_store_op_t op = {key, data, size, false};
  return gw_store_transaction(&op, 1);
}
esp_err_t gw_store_delete(const char *key) {
  gw_store_op_t op = {.key = key, .remove = true};
  return gw_store_transaction(&op, 1);
}
esp_err_t gw_store_put_json(const char *key, const cJSON *value) {
  char *json = cJSON_PrintUnformatted(value);
  if (!json)
    return ESP_ERR_NO_MEM;
  esp_err_t err = gw_store_put(key, json, strlen(json));
  free(json);
  return err;
}
cJSON *gw_store_get_json(const char *key) {
  size_t size = 0;
  if (gw_store_get(key, NULL, &size) != ESP_OK)
    return NULL;
  char *data = malloc(size + 1);
  if (!data)
    return NULL;
  if (gw_store_get(key, data, &size) != ESP_OK) {
    free(data);
    return NULL;
  }
  data[size] = 0;
  cJSON *result = cJSON_Parse(data);
  free(data);
  return result;
}
esp_err_t gw_factory_get_string(const char *name, char *out, size_t size) {
  if (!factory_handle || !name || !out || !size)
    return ESP_ERR_INVALID_STATE;
  return nvs_get_str(factory_handle, name, out, &size);
}
esp_err_t gw_factory_get_blob(const char *name, void *out, size_t *size) {
  if (!factory_handle || !name || !size)
    return ESP_ERR_INVALID_STATE;
  return nvs_get_blob(factory_handle, name, out, size);
}
esp_err_t gw_secret_get(const char *key, void *data, size_t *size) {
  if (!store_lock || !secret_handle || !key || strlen(key) > 15 || !size)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(store_lock, portMAX_DELAY);
  esp_err_t e = nvs_get_blob(secret_handle, key, data, size);
  xSemaphoreGive(store_lock);
  return e;
}
esp_err_t gw_secret_put(const char *key, const void *data, size_t size) {
  if (!store_lock || !secret_handle || !key || strlen(key) > 15 || !data ||
      !size || size > 4096)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(store_lock, portMAX_DELAY);
  esp_err_t e = store_fault ? ESP_ERR_INVALID_STATE
                            : nvs_set_blob(secret_handle, key, data, size);
  if (e == ESP_OK)
    e = nvs_commit(secret_handle);
  if (e == ESP_OK) {
    size_t got = size;
    void *check = malloc(size);
    if (!check)
      e = ESP_ERR_NO_MEM;
    else {
      e = nvs_get_blob(secret_handle, key, check, &got);
      if (e == ESP_OK && (got != size || memcmp(data, check, size)))
        e = ESP_FAIL;
      memset(check, 0, size);
      free(check);
    }
  }
  if (e != ESP_OK)
    store_fault = true;
  xSemaphoreGive(store_lock);
  return e;
}
