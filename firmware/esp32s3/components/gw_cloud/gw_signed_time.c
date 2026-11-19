#include "gw_signed_time.h"
#include "esp_random.h"
#include "gw_core.h"
#include "gw_http.h"
#include "mbedtls/base64.h"
#include <string.h>
esp_err_t gw_signed_time_refresh(const char *boot_id) {
  char key_id[65], ppm[8];
  uint8_t key[32], random[32];
  size_t n = sizeof(key);
  if (gw_factory_get_string("time_key_id", key_id, sizeof(key_id)) != ESP_OK ||
      gw_factory_get_string("time_ppm", ppm, sizeof(ppm)) != ESP_OK ||
      strcmp(ppm, "100") ||
      gw_factory_get_blob("time_pub", key, &n) != ESP_OK || n != sizeof(key))
    return ESP_ERR_INVALID_STATE;
  esp_fill_random(random, sizeof(random));
  unsigned char nonce[45];
  size_t encoded;
  if (mbedtls_base64_encode(nonce, sizeof(nonce), &encoded, random,
                            sizeof(random)))
    return ESP_FAIL;
  nonce[encoded] = 0;
  uint64_t sent = gw_core_mono_ms();
  cJSON *response = NULL;
  esp_err_t e = gw_http_time_challenge((const char *)nonce, boot_id,
                                       sent + 2000, &response);
  uint64_t received = gw_core_mono_ms(), issued;
  uint32_t uncertainty;
  if (e == ESP_OK)
    e = gw_signed_time_verify(response, key_id, key, (char *)nonce, boot_id,
                              &issued, &uncertainty);
  if (e == ESP_OK)
    e = gw_core_time_accept(issued, uncertainty, sent, received);
  cJSON_Delete(response);
  return e;
}
