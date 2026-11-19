#include "gw_ble_bonds.h"
#include "host/ble_store.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

static bool same_material(const struct ble_store_value_sec *a,
                          const struct ble_store_value_sec *b) {
  return a->peer_addr.type == b->peer_addr.type &&
         !memcmp(a->peer_addr.val, b->peer_addr.val, 6) &&
         a->key_size == b->key_size && a->ediv == b->ediv &&
         a->rand_num == b->rand_num && a->ltk_present == b->ltk_present &&
         !memcmp(a->ltk, b->ltk, 16) && a->irk_present == b->irk_present &&
         !memcmp(a->irk, b->irk, 16) && a->csrk_present == b->csrk_present &&
         !memcmp(a->csrk, b->csrk, 16) && a->sign_counter == b->sign_counter &&
         a->authenticated == b->authenticated && a->sc == b->sc;
}

esp_err_t gw_ble_bonds_check(bool restored) {
#if CONFIG_BT_NIMBLE_NVS_PERSIST
  nvs_handle_t handle;
  esp_err_t result = nvs_open("nimble_bond", NVS_READWRITE, &handle);
  if (result != ESP_OK)
    return result;
  static const char *prefix[] = {"our_sec", "peer_sec"};
  static const int kind[] = {BLE_STORE_OBJ_TYPE_OUR_SEC,
                             BLE_STORE_OBJ_TYPE_PEER_SEC};
  struct ble_store_value_sec stored = {0}, actual = {0};
  for (unsigned side = 0; side < 2; side++) {
    int expected_count = 0;
    for (unsigned i = 1; i <= CONFIG_BT_NIMBLE_MAX_BONDS; i++) {
      char name[16];
      snprintf(name, sizeof(name), "%s_%u", prefix[side], i);
      size_t size = sizeof(stored);
      result = nvs_get_blob(handle, name, &stored, &size);
      if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
        continue;
      }
      if (result != ESP_OK)
        goto done;
      if (size != sizeof(stored) || stored.peer_addr.type > 1 ||
          (stored.ltk_present &&
           (stored.key_size < 7 || stored.key_size > 16))) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
      }
      expected_count++;
      if (restored) {
        struct ble_store_key_sec key = {0};
        key.peer_addr = stored.peer_addr;
        int read_result = side ? ble_store_read_peer_sec(&key, &actual)
                               : ble_store_read_our_sec(&key, &actual);
        if (read_result || !same_material(&stored, &actual)) {
          result = ESP_ERR_INVALID_RESPONSE;
          goto done;
        }
      }
      mbedtls_platform_zeroize(&stored, sizeof(stored));
      mbedtls_platform_zeroize(&actual, sizeof(actual));
    }
    if (restored) {
      int count = 0;
      if (ble_store_util_count(kind[side], &count) || count != expected_count) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto done;
      }
    }
  }
done:
  mbedtls_platform_zeroize(&stored, sizeof(stored));
  mbedtls_platform_zeroize(&actual, sizeof(actual));
  nvs_close(handle);
  return result;
#else
  (void)restored;
  return ESP_ERR_NOT_SUPPORTED;
#endif
}
