#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_actions.h"
#include "gw_capture.h"
#include "gw_cloud.h"
#include "gw_core.h"
#include "gw_nrf.h"
#include "gw_provision.h"
#include "sdkconfig.h"

static const char *TAG = "gw_boot";

void app_main(void) {
  esp_err_t err = gw_core_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG,
             "Core/storage provisioning unavailable (%s); no automatic erase",
             esp_err_to_name(err));
    esp_ota_img_states_t image_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                    &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY)
      (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    return;
  }
  err = gw_capture_init();
  if (err == ESP_OK)
    err = gw_nrf_init(gw_capture_nrf, NULL);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "BLE custody unavailable (%s)", esp_err_to_name(err));
  err = gw_actions_init();
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Radio/action recovery unavailable (%s)",
             esp_err_to_name(err));
  err = gw_provision_init();
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Local provisioning unavailable (%s)", esp_err_to_name(err));
  err = gw_cloud_init();
  if (err != ESP_OK)
    ESP_LOGW(TAG,
             "Cloud unavailable (%s); local provisioning remains available",
             esp_err_to_name(err));
#if CONFIG_GW_FACTORY_AUTHORIZED_SETUP
  cJSON *current = gw_core_state();
  const cJSON *network =
      cJSON_GetObjectItemCaseSensitive(current, "desiredLocalNetwork");
  uint8_t used = 0;
  size_t used_size = sizeof(used);
  if (!cJSON_IsObject(network) &&
      gw_store_get("core/setup-used", &used, &used_size) == ESP_ERR_NOT_FOUND) {
    used = 1;
    if (gw_store_put("core/setup-used", &used, sizeof(used)) == ESP_OK)
      gw_provision_open_window(600);
  }
  cJSON_Delete(current);
#endif
#if CONFIG_GW_SETUP_BUTTON_GPIO >= 0
  gpio_config_t button = {.pin_bit_mask = 1ULL << CONFIG_GW_SETUP_BUTTON_GPIO,
                          .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE,
                          .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE};
  if (gpio_config(&button) != ESP_OK) {
    ESP_LOGE(TAG, "Configured setup input unavailable");
    return;
  }
  uint64_t pressed = 0;
  bool handled = false;
  for (;;) {
    if (!gpio_get_level(CONFIG_GW_SETUP_BUTTON_GPIO)) {
      if (!pressed)
        pressed = gw_core_mono_ms();
      if (!handled && gw_core_mono_ms() - pressed >= 3000) {
        err = gw_provision_open_window(600);
        if (err != ESP_OK)
          ESP_LOGW(TAG, "Setup window rejected (%s)", esp_err_to_name(err));
        handled = true;
      }
    } else {
      pressed = 0;
      handled = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
#endif
}
