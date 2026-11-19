#include "../gw_ble_bonds.h"
#include "host/ble_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool record, open_failed, read_failed, load_failed, changed, bad_length;
static unsigned closed;
static struct ble_store_value_sec saved;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) {
  assert(!strcmp(ns, "nimble_bond") && mode == NVS_READWRITE);
  *handle = 1;
  return open_failed ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                       size_t *size) {
  assert(handle == 1);
  if (!record || strcmp(key, "peer_sec_1"))
    return ESP_ERR_NVS_NOT_FOUND;
  if (read_failed)
    return ESP_FAIL;
  assert(*size >= sizeof(saved));
  memcpy(out, &saved, sizeof(saved));
  *size = sizeof(saved) - (bad_length ? 1 : 0);
  return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
  assert(handle == 1);
  closed++;
}
int ble_store_read_our_sec(const struct ble_store_key_sec *k,
                           struct ble_store_value_sec *v) {
  (void)k;
  (void)v;
  return 1;
}
int ble_store_read_peer_sec(const struct ble_store_key_sec *k,
                            struct ble_store_value_sec *v) {
  assert(!memcmp(k->peer_addr.val, saved.peer_addr.val, 6));
  if (load_failed)
    return 1;
  *v = saved;
  if (changed)
    v->ltk[0] ^= 1;
  return 0;
}
int ble_store_util_count(int kind, int *count) {
  *count = (kind == BLE_STORE_OBJ_TYPE_PEER_SEC && record) ? 1 : 0;
  return 0;
}
void mbedtls_platform_zeroize(void *p, size_t n) {
  volatile unsigned char *v = p;
  while (n--)
    *v++ = 0;
}
int main(void) {
  assert(gw_ble_bonds_check(false) == ESP_OK &&
         gw_ble_bonds_check(true) == ESP_OK);
  open_failed = true;
  unsigned previous = closed;
  assert(gw_ble_bonds_check(false) == ESP_FAIL && closed == previous);
  open_failed = false;
  record = true;
  saved.peer_addr.type = 1;
  saved.peer_addr.val[0] = 7;
  saved.key_size = 16;
  saved.ltk_present = 1;
  saved.ltk[0] = 9;
  assert(gw_ble_bonds_check(false) == ESP_OK &&
         gw_ble_bonds_check(true) == ESP_OK);
  read_failed = true;
  assert(gw_ble_bonds_check(false) == ESP_FAIL);
  read_failed = false;
  bad_length = true;
  assert(gw_ble_bonds_check(false) == ESP_ERR_INVALID_RESPONSE);
  bad_length = false;
  saved.key_size = 6;
  assert(gw_ble_bonds_check(false) == ESP_ERR_INVALID_RESPONSE);
  saved.key_size = 16;
  load_failed = true;
  assert(gw_ble_bonds_check(true) == ESP_ERR_INVALID_RESPONSE);
  load_failed = false;
  changed = true;
  assert(gw_ble_bonds_check(true) == ESP_ERR_INVALID_RESPONSE);
  changed = false;
  assert(gw_ble_bonds_check(true) == ESP_OK);
  puts("PASS: encrypted bond NVS preflight, empty store, read corruption and "
       "restored-key mismatch");
  return 0;
}
