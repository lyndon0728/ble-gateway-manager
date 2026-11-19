#include "gw_nrf.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_authority.h"
#include "gw_radio_evidence.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT UART_NUM_1
#if defined(CONFIG_GW_NRF_RESET_BOARD_VERIFIED) &&                             \
    CONFIG_GW_NRF_RESET_BOARD_VERIFIED
#define RESET_PIN CONFIG_GW_NRF_RESET_GPIO
#else
#define RESET_PIN 8
#endif
typedef struct {
  uint8_t boot[16];
  struct gw_frame frame;
} batch_t;
static SemaphoreHandle_t request_lock, tx_lock, identity_lock, io_lock;
static QueueHandle_t responses, batches, events;
static gw_nrf_batch_fn receive_batch;
static void *receive_context;
static uint64_t link_id;
static uint32_t next_id = 1, capabilities;
static uint8_t boot_id[16];
static char firmware[64];
static bool ready;
static uint64_t bad_frames, busy_batches;

int64_t gw_nrf_now_ms(void) { return esp_timer_get_time() / 1000; }
static TickType_t remaining(int64_t deadline) {
  int64_t ms = deadline - gw_nrf_now_ms();
  return ms <= 0 ? 0 : pdMS_TO_TICKS(ms) + 1;
}
static esp_err_t transmit(const struct gw_frame *f, int64_t deadline) {
  uint8_t *wire = malloc(GW_WIRE_MAX);
  if (!wire)
    return ESP_ERR_NO_MEM;
  size_t n = gw_encode_frame(f, wire, GW_WIRE_MAX);
  if (!n) {
    free(wire);
    return ESP_ERR_INVALID_ARG;
  }
  if (!remaining(deadline) ||
      xSemaphoreTake(tx_lock, remaining(deadline)) != pdTRUE) {
    free(wire);
    return ESP_ERR_TIMEOUT;
  }
  /* IDF TX ring owns a copy after uart_write_bytes; mutex extends through
   * completion. No next frame starts while old queued bytes remain. */
  esp_err_t e = ESP_FAIL;
  if (remaining(deadline) && uart_write_bytes(PORT, wire, n) == (int)n)
    e = uart_wait_tx_done(PORT, remaining(deadline));
  if (e != ESP_OK) {
    /* No TX buffer cancellation API: stop/restart is not safe here.
     * Hold serialization until all queued bytes drain; CTS watchdog is
     * handled by a bounded driver reset under this exclusive lock. */
    xSemaphoreTake(io_lock, portMAX_DELAY);
    uart_driver_delete(PORT);
    esp_err_t installed = uart_driver_install(PORT, 8192, 4096, 24, &events, 0);
    if (installed != ESP_OK)
      events = NULL;
    uart_set_hw_flow_ctrl(PORT, UART_HW_FLOWCTRL_CTS_RTS, 96);
    xSemaphoreGive(io_lock);
    xSemaphoreTake(identity_lock, portMAX_DELAY);
    ready = false;
    xSemaphoreGive(identity_lock);
  }
  xSemaphoreGive(tx_lock);
  free(wire);
  return e;
}
bool gw_nrf_identity(uint8_t out[16], uint32_t *caps, char version[64]) {
  if (!identity_lock)
    return false;
  xSemaphoreTake(identity_lock, portMAX_DELAY);
  bool valid = ready;
  if (out)
    memcpy(out, boot_id, 16);
  if (caps)
    *caps = capabilities;
  if (version)
    memcpy(version, firmware, 64);
  xSemaphoreGive(identity_lock);
  return valid;
}
static void rx_task(void *unused) {
  (void)unused;
  struct gw_decoder *decoder = calloc(1, sizeof(*decoder));
  struct gw_frame *f = malloc(sizeof(*f));
  uint8_t bytes[256];
  if (!decoder || !f) {
    free(decoder);
    free(f);
    vTaskDelete(NULL);
    return;
  }
  for (;;) {
    xSemaphoreTake(io_lock, portMAX_DELAY);
    int n = events
                ? uart_read_bytes(PORT, bytes, sizeof(bytes), pdMS_TO_TICKS(20))
                : 0;
    uart_event_t event;
    while (events && xQueueReceive(events, &event, 0) == pdTRUE) {
      if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
          event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR) {
        ++bad_frames;
        decoder->length = 0;
        decoder->discard = true;
      }
    }
    xSemaphoreGive(io_lock);
    if (!n) {
      vTaskDelay(1);
      continue;
    }
    for (int i = 0; i < n; ++i) {
      int rc = gw_decoder_feed(decoder, bytes[i], f);
      if (rc < 0) {
        ++bad_frames;
        continue;
      }
      if (rc != 1)
        continue;
      xSemaphoreTake(identity_lock, portMAX_DELAY);
      bool match = f->link_id == link_id;
      bool online = ready;
      xSemaphoreGive(identity_lock);
      if (!match)
        continue;
      if (f->type == 0x81 || f->type == 0x90) {
        /* A response belongs to the sole in-flight request. */
        (void)xQueueOverwrite(responses, f);
      } else if (online && f->type >= 0x20 && f->type <= 0x22) {
        batch_t *b = malloc(sizeof(*b));
        if (b) {
          gw_nrf_identity(b->boot, NULL, NULL);
          b->frame = *f;
          if (xQueueSend(batches, &b, 0) == pdTRUE)
            continue;
          free(b);
        }
        ++busy_batches;
        /* Peer keeps its frozen batch and retries; withholding ACK
         * under RAM pressure is safe and avoids blocking RX. */
      }
    }
  }
}
static void batch_task(void *unused) {
  (void)unused;
  for (;;) {
    batch_t *b;
    if (xQueueReceive(batches, &b, portMAX_DELAY) != pdTRUE)
      continue;
    uint8_t status =
        receive_batch ? receive_batch(b->boot, &b->frame, receive_context) : 1;
    if (status == 255) {
      free(b);
      continue;
    }
    if (status > 2)
      status = 2;
    struct gw_frame *ack = calloc(1, sizeof(*ack));
    if (ack) {
      ack->type = 0xa0;
      ack->link_id = b->frame.link_id;
      ack->msg_id = b->frame.msg_id;
      ack->payload_len = 3;
      ack->payload[0] = status;
      gw_put_u16(ack->payload + 1, status == 1 ? 500 : 0);
      transmit(ack, gw_nrf_now_ms() + 500);
      free(ack);
    }
    free(b);
  }
}
esp_err_t gw_nrf_rehandshake(int64_t deadline) {
  if (!request_lock || !remaining(deadline) ||
      xSemaphoreTake(request_lock, remaining(deadline)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  struct gw_frame *f = calloc(1, sizeof(*f)), *r = malloc(sizeof(*r));
  esp_err_t e = ESP_ERR_NO_MEM;
  if (!f || !r)
    goto done;
  xSemaphoreTake(identity_lock, portMAX_DELAY);
  esp_fill_random(&link_id, sizeof(link_id));
  if (!link_id)
    link_id = 1;
  f->link_id = link_id;
  ready = false;
  xSemaphoreGive(identity_lock);
  f->type = 1;
  next_id = 1;
  xQueueReset(responses);
  for (uint32_t backoff = 250; remaining(deadline);
       backoff = backoff < 2000 ? backoff * 2 : 2000) {
    e = transmit(f, deadline);
    if (e != ESP_OK)
      break;
    int64_t until = gw_nrf_now_ms() + backoff;
    if (until > deadline)
      until = deadline;
    while (remaining(until) &&
           xQueueReceive(responses, r, remaining(until)) == pdTRUE) {
      if (r->type != 0x81 || r->msg_id || r->link_id != f->link_id)
        continue;
      if (r->payload_len < 22 || r->payload[20] < 1 || r->payload[20] > 63 ||
          r->payload_len != (size_t)(21 + r->payload[20]))
        continue;
      xSemaphoreTake(identity_lock, portMAX_DELAY);
      memcpy(boot_id, r->payload, 16);
      capabilities = gw_get_u32(r->payload + 16);
      memset(firmware, 0, sizeof(firmware));
      memcpy(firmware, r->payload + 21, r->payload[20]);
      ready = true;
      xSemaphoreGive(identity_lock);
      e = ESP_OK;
      goto done;
    }
    e = ESP_ERR_TIMEOUT;
  }
done:
  free(f);
  free(r);
  xSemaphoreGive(request_lock);
  return e;
}
esp_err_t gw_nrf_command(uint8_t op, const void *args, size_t size,
                         uint8_t *status, void *result, size_t *out_size,
                         int64_t deadline) {
  if (size > GW_PAYLOAD_MAX - 1 || (size && !args) || !out_size || !status)
    return ESP_ERR_INVALID_ARG;
  if (!request_lock || !remaining(deadline) ||
      xSemaphoreTake(request_lock, remaining(deadline)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  esp_err_t e = ESP_ERR_INVALID_STATE;
  struct gw_frame *f = NULL, *r = NULL;
  if (!gw_nrf_identity(NULL, NULL, NULL) || next_id == UINT32_MAX)
    goto done;
  f = calloc(1, sizeof(*f));
  r = malloc(sizeof(*r));
  if (!f || !r) {
    e = ESP_ERR_NO_MEM;
    goto done;
  }
  f->type = 0x10;
  f->link_id = link_id;
  f->msg_id = next_id++;
  f->payload_len = 1 + size;
  f->payload[0] = op;
  if (size)
    memcpy(f->payload + 1, args, size);
  xQueueReset(responses);
  /* TIME_PROBE is deliberately never retransmitted: its reply cannot then
   * be mistaken for the original timing sample. */
  for (unsigned attempt = 0;
       attempt < (op == 4 ? 1U : 3U) && remaining(deadline); ++attempt) {
    e = transmit(f, deadline);
    if (e != ESP_OK)
      break;
    int64_t until = gw_nrf_now_ms() + (op >= 0x30 && op <= 0x37 ? 30000 : 500);
    if (until > deadline)
      until = deadline;
    while (remaining(until) &&
           xQueueReceive(responses, r, remaining(until)) == pdTRUE) {
      if (r->type != 0x90 || r->link_id != f->link_id ||
          r->msg_id != f->msg_id || r->payload_len < 2 || r->payload[0] != op)
        continue;
      *status = r->payload[1];
      if (*status > 7 || (*status && r->payload_len != 2))
        continue;
      size_t n = r->payload_len - 2;
      if (n > *out_size || (n && !result)) {
        e = ESP_ERR_INVALID_SIZE;
        goto done;
      }
      if (n)
        memcpy(result, r->payload + 2, n);
      *out_size = n;
      e = ESP_OK;
      goto done;
    }
    e = ESP_ERR_TIMEOUT;
  }
  if (e != ESP_OK) {
    xSemaphoreTake(identity_lock, portMAX_DELAY);
    ready = false;
    xSemaphoreGive(identity_lock);
  }
done:
  free(f);
  free(r);
  xSemaphoreGive(request_lock);
  return e;
}
static void monitor(void *unused) {
  (void)unused;
  for (;;) {
    if (!gw_nrf_identity(NULL, NULL, NULL))
      gw_nrf_rehandshake(gw_nrf_now_ms() + 2500);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
esp_err_t gw_nrf_init(gw_nrf_batch_fn callback, void *ctx) {
  request_lock = xSemaphoreCreateMutex();
  tx_lock = xSemaphoreCreateMutex();
  identity_lock = xSemaphoreCreateMutex();
  io_lock = xSemaphoreCreateMutex();
  responses = xQueueCreate(1, sizeof(struct gw_frame));
  batches = xQueueCreate(4, sizeof(batch_t *));
  if (!request_lock || !tx_lock || !identity_lock || !io_lock || !responses ||
      !batches)
    return ESP_ERR_NO_MEM;
  receive_batch = callback;
  receive_context = ctx;
  uart_config_t cfg = {.baud_rate = 1000000,
                       .data_bits = UART_DATA_8_BITS,
                       .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1,
                       .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
                       .rx_flow_ctrl_thresh = 96,
                       .source_clk = UART_SCLK_DEFAULT};
  esp_err_t e = uart_param_config(PORT, &cfg);
  if (e)
    return e;
  e = uart_set_pin(PORT, 17, 18, 15, 16);
  if (e)
    return e;
  /* Release reset. No reset is performed just because a serial handshake
   * failed. */
  gpio_config_t reset = {.pin_bit_mask = 1ULL << RESET_PIN,
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_DISABLE,
                         .pull_down_en = GPIO_PULLDOWN_DISABLE};
  if ((e = gpio_config(&reset)) != ESP_OK)
    return e;
  if ((e = uart_driver_install(PORT, 8192, 4096, 24, &events, 0)) != ESP_OK)
    return e;
  if (xTaskCreate(rx_task, "nrf_rx", 10240, NULL, 12, NULL) != pdPASS ||
      xTaskCreate(batch_task, "nrf_custody", 8192, NULL, 8, NULL) != pdPASS ||
      xTaskCreate(monitor, "nrf_link", 8192, NULL, 6, NULL) != pdPASS)
    return ESP_ERR_NO_MEM;
  return ESP_OK;
}

/* Configuration conversion is kept separate from the transport. */
static double number(const cJSON *o, const char *key, double fallback) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}
static bool b64(const cJSON *o, const char *key, uint8_t *out, size_t cap,
                size_t *n) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsString(v))
    return false;
  return mbedtls_base64_decode(out, cap, n,
                               (const unsigned char *)v->valuestring,
                               strlen(v->valuestring)) == 0;
}
static esp_err_t set_readback(uint8_t set, uint8_t get, const uint8_t *data,
                              size_t n, int64_t deadline) {
  uint8_t *result = malloc(GW_PAYLOAD_MAX);
  if (!result)
    return ESP_ERR_NO_MEM;
  uint8_t status = 0xff;
  size_t sz = GW_PAYLOAD_MAX;
  esp_err_t e = gw_nrf_command(set, data, n, &status, result, &sz, deadline);
  if (e == ESP_OK && (status || sz != n || memcmp(result, data, n)))
    e = ESP_FAIL;
  if (e == ESP_OK) {
    sz = GW_PAYLOAD_MAX;
    e = gw_nrf_command(get, NULL, 0, &status, result, &sz, deadline);
    if (e == ESP_OK && (status || sz != n || memcmp(result, data, n)))
      e = ESP_FAIL;
  }
  free(result);
  return e;
}
esp_err_t gw_nrf_scan(const cJSON *complete, uint64_t revision,
                      int64_t deadline) {
  uint8_t *data = malloc(3070);
  if (!data)
    return ESP_ERR_NO_MEM;
  size_t n = 0;
  esp_err_t e = gw_nrf_compile_scan(complete, revision, data, 3070, &n);
  if (e == ESP_OK && data[1] && !gw_authority_has("NRF54_BLE_FILTER_V2"))
    e = ESP_ERR_NOT_SUPPORTED;
  uint32_t caps = 0;
  if (e == ESP_OK &&
      (!gw_nrf_identity(NULL, &caps, NULL) || !(caps & (1u << 1))))
    e = ESP_ERR_NOT_SUPPORTED;
  if (e == ESP_OK) {
    if (caps & (1u << 13)) {
      e = set_readback(0x19, 0x1a, data, n, deadline);
      if (e == ESP_OK) {
        uint8_t result[96], status;
        size_t length = sizeof(result);
        e = gw_nrf_command(0x1b, NULL, 0, &status, result, &length, deadline);
        if (e == ESP_OK &&
            (status || length != 96 || gw_get_u64(result) != revision ||
             result[8] != data[1] ||
             result[9] != data[4 + gw_get_u16(data + 2)]))
          e = ESP_ERR_INVALID_RESPONSE;
      }
    } else if (data[1])
      e = ESP_ERR_NOT_SUPPORTED;
    else
      e = set_readback(5, 6, data + 4, gw_get_u16(data + 2), deadline);
  }
  free(data);
  return e;
}
esp_err_t gw_nrf_advertise(const cJSON *adv, uint64_t revision,
                           int64_t deadline) {
  uint8_t data[76];
  size_t n = 0, m = 0;
  uint32_t caps = 0;
  gw_nrf_identity(NULL, &caps, NULL);
  if (!(caps & (1U << 4)))
    return ESP_ERR_NOT_SUPPORTED;
  const cJSON *mode = cJSON_GetObjectItemCaseSensitive(adv, "mode");
  if (!cJSON_IsString(mode) || !revision)
    return ESP_ERR_INVALID_ARG;
  uint8_t kind = !strcmp(mode->valuestring, "NONCONNECTABLE")             ? 0
                 : !strcmp(mode->valuestring, "SCANNABLE_NONCONNECTABLE") ? 1
                 : !strcmp(mode->valuestring, "CONNECTABLE_SCANNABLE")    ? 2
                                                                          : 255;
  if (kind == 255 || (kind == 2 && !(caps & (1U << 11))))
    return ESP_ERR_NOT_SUPPORTED;
  double interval = number(adv, "intervalMs", 100);
  if (interval < 100 || interval > 10240 || floor(interval) != interval)
    return ESP_ERR_INVALID_ARG;
  gw_put_u64(data, revision);
  data[8] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(adv, "enabled"));
  data[9] = kind;
  gw_put_u16(data + 10, ((uint32_t)interval * 8 + 2) / 5);
  if (!b64(adv, "advDataBase64", data + 13, 31, &n) ||
      !gw_ad_valid(data + 13, n))
    return ESP_ERR_INVALID_ARG;
  data[12] = n;
  if (!b64(adv, "scanResponseBase64", data + 14 + n, 31, &m) ||
      !gw_ad_valid(data + 14 + n, m) || (kind == 0 && m))
    return ESP_ERR_INVALID_ARG;
  data[13 + n] = m;
  esp_err_t e = set_readback(7, 8, data, 14 + n + m, deadline);
  if (e != ESP_OK)
    return e;
  uint8_t result[35], status;
  size_t length = sizeof(result);
  if (caps & (1u << 10)) {
    e = gw_nrf_command(0x0c, NULL, 0, &status, result, &length, deadline);
    if (e != ESP_OK)
      return e;
    if (status)
      return ESP_ERR_INVALID_RESPONSE;
    e = gw_advertising_evidence(data, 14 + n + m, result, length);
    if (e != ESP_ERR_NOT_FINISHED)
      return e;
    /* PAUSED is successful only with a live peripheral, not merely enabled=1.
     */
    length = 16;
    e = gw_nrf_command(0x0d, NULL, 0, &status, result, &length, deadline);
    return e != ESP_OK ? e
           : (!status && gw_peripheral_live(result, length))
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
  }
  length = sizeof(result);
  e = gw_nrf_command(9, NULL, 0, &status, result, &length, deadline);
  if (e != ESP_OK)
    return e;
  return !status && length == 15 && gw_get_u64(result + 2) == revision &&
                 result[1] == (data[8] ? 1 : 0)
             ? ESP_OK
             : ESP_ERR_INVALID_STATE;
}

esp_err_t gw_nrf_hardware_reset(int64_t deadline) {
#if defined(CONFIG_GW_NRF_RESET_BOARD_VERIFIED) &&                             \
    CONFIG_GW_NRF_RESET_BOARD_VERIFIED
  if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_GW_NRF_RESET_GPIO))
    return ESP_ERR_INVALID_ARG;
  if (!request_lock || remaining(deadline) == 0 ||
      xSemaphoreTake(request_lock, remaining(deadline)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  uint8_t previous[16] = {0};
  (void)gw_nrf_identity(previous, NULL, NULL);
  bool known = false;
  for (unsigned i = 0; i < 16; i++)
    known |= previous[i] != 0;
  esp_err_t e = ESP_ERR_TIMEOUT;
  if (deadline - gw_nrf_now_ms() <=
      CONFIG_GW_NRF_RESET_LOW_MS + CONFIG_GW_NRF_RESET_SETTLE_MS)
    goto reset_done;
  xSemaphoreTake(identity_lock, portMAX_DELAY);
  ready = false;
  xSemaphoreGive(identity_lock);
  /* Set the output latch before switching direction; never drive a shared reset
   * high. */
  e = gpio_set_level(CONFIG_GW_NRF_RESET_GPIO, 0);
  if (e == ESP_OK)
    e = gpio_set_direction(CONFIG_GW_NRF_RESET_GPIO, GPIO_MODE_OUTPUT_OD);
  if (e == ESP_OK)
    vTaskDelay(pdMS_TO_TICKS(CONFIG_GW_NRF_RESET_LOW_MS) + 1);
  esp_err_t released =
      gpio_set_direction(CONFIG_GW_NRF_RESET_GPIO, GPIO_MODE_INPUT);
  if (e == ESP_OK)
    e = released;
  if (e == ESP_OK)
    vTaskDelay(pdMS_TO_TICKS(CONFIG_GW_NRF_RESET_SETTLE_MS) + 1);
reset_done:
  xSemaphoreGive(request_lock);
  if (e != ESP_OK)
    return e;
  e = gw_nrf_rehandshake(deadline);
  uint8_t current[16];
  if (e == ESP_OK && (!gw_nrf_identity(current, NULL, NULL) ||
                      (known && !memcmp(previous, current, 16))))
    return ESP_ERR_INVALID_RESPONSE;
  return e;
#else
  (void)deadline;
  return ESP_ERR_NOT_SUPPORTED;
#endif
}
cJSON *gw_nrf_diagnostics(int64_t deadline) {
  cJSON *o = cJSON_CreateObject();
  uint8_t boot[16], result[96], status;
  uint32_t caps = 0;
  char version[64] = {0};
  size_t n = sizeof(result);
  if (!o)
    return NULL;
  cJSON_AddBoolToObject(o, "online", gw_nrf_identity(boot, &caps, version));
  cJSON_AddNumberToObject(o, "badFrames", bad_frames);
  cJSON_AddNumberToObject(o, "busyBatches", busy_batches);
  cJSON_AddNumberToObject(o, "capabilities", caps);
  cJSON_AddStringToObject(o, "firmwareVersion", version);
  if (gw_nrf_command(3, NULL, 0, &status, result, &n, deadline) == ESP_OK &&
      !status && n == 71) {
    cJSON_AddNumberToObject(o, "queuedRecords", gw_get_u16(result + 17));
    cJSON_AddNumberToObject(o, "queuedBytes", gw_get_u32(result + 19));
    cJSON_AddNumberToObject(o, "scanState", result[16]);
  }
  if (caps & (1u << 10)) {
    n = sizeof(result);
    if (gw_nrf_command(0x0c, NULL, 0, &status, result, &n, deadline) ==
            ESP_OK &&
        !status && n == 35) {
      static const char *states[] = {"STOPPED", "ACTIVE", "FAULT", "PAUSED"};
      char revision[24];
      snprintf(revision, sizeof(revision), "%llu",
               (unsigned long long)gw_get_u64(result));
      cJSON_AddStringToObject(o, "advertisingRevision", revision);
      cJSON_AddStringToObject(o, "advertisingState",
                              result[9] < 4 ? states[result[9]] : "UNKNOWN");
      cJSON_AddBoolToObject(o, "advertisingEnabled", result[8] != 0);
      cJSON_AddNumberToObject(o, "advertisingDriverError",
                              (int32_t)gw_get_u32(result + 15));
      if (result[9] == 3) {
        n = sizeof(result);
        if (gw_nrf_command(0x0d, NULL, 0, &status, result, &n, deadline) ==
                ESP_OK &&
            !status && gw_peripheral_live(result, n))
          cJSON_AddStringToObject(o, "advertisingPauseReason",
                                  "PERIPHERAL_CONNECTION");
      }
    }
  }
  if (!(caps & (1u << 10)) && (caps & (1u << 4))) {
    n = sizeof(result);
    if (gw_nrf_command(9, NULL, 0, &status, result, &n, deadline) == ESP_OK &&
        !status && n == 15) {
      static const char *states[] = {"STOPPED", "ACTIVE", "FAULT", "PAUSED"};
      char revision[24];
      snprintf(revision, sizeof(revision), "%llu",
               (unsigned long long)gw_get_u64(result + 2));
      cJSON_AddStringToObject(o, "advertisingRevision", revision);
      cJSON_AddStringToObject(o, "advertisingState",
                              result[1] < 4 ? states[result[1]] : "UNKNOWN");
    }
  }
  if (caps & (1u << 13)) {
    n = sizeof(result);
    if (gw_nrf_command(0x1b, NULL, 0, &status, result, &n, deadline) ==
            ESP_OK &&
        !status && n == 96) {
      cJSON *f = cJSON_AddObjectToObject(o, "scanFilterStatus");
      char counter[24];
      snprintf(counter, sizeof(counter), "%llu",
               (unsigned long long)gw_get_u64(result));
      cJSON_AddStringToObject(f, "revision", counter);
      cJSON_AddStringToObject(f, "engine", result[8] ? "V2" : "LEGACY");
      cJSON_AddStringToObject(f, "scope",
                              result[9] ? "ADV_SCAN_RSP_PAIR" : "REPORT");
      cJSON_AddNumberToObject(f, "pendingCount", gw_get_u16(result + 10));
      cJSON_AddNumberToObject(f, "pendingBytes", gw_get_u32(result + 12));
      static const char *names[] = {
          "recordsSeen",        "recordsPassed",  "ruleDropped", "pairTimeout",
          "pairAmbiguous",      "pairOverflow",   "malformedAd", "unknownRssi",
          "unsupportedContext", "configDiscarded"};
      for (unsigned i = 0; i < 10; i++) {
        snprintf(counter, sizeof(counter), "%llu",
                 (unsigned long long)gw_get_u64(result + 16 + 8 * i));
        cJSON_AddStringToObject(f, names[i], counter);
      }
    }
  }
  return o;
}
