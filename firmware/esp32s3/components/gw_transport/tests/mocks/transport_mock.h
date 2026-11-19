#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum {
  ESP_OK,
  ESP_FAIL,
  ESP_ERR_INVALID_ARG,
  ESP_ERR_NO_MEM,
  ESP_ERR_INVALID_STATE,
  ESP_ERR_TIMEOUT,
  ESP_ERR_INVALID_SIZE
};
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, uint64_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(unsigned);
uint64_t gw_core_mono_ms(void);
bool gw_core_time_trusted(void);
esp_err_t gw_factory_get_string(const char *, char *, size_t);
esp_err_t gw_http_json(const char *, const char *, const cJSON *, const char *,
                       uint64_t, cJSON **);
esp_err_t gw_http_tls_prepare(void);
esp_err_t gw_http_tls_attach(void *);
typedef void *esp_mqtt_client_handle_t;
typedef const char *esp_event_base_t;
typedef struct {
  int msg_id, retain, qos, current_data_offset, total_data_len, topic_len,
      data_len;
  char *topic, *data;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
enum {
  MQTT_EVENT_CONNECTED,
  MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED,
  MQTT_EVENT_DATA,
  MQTT_EVENT_PUBLISHED
};
#define ESP_EVENT_ANY_ID -1
#define MQTT_PROTOCOL_V_3_1_1 4
typedef struct {
  struct {
    struct {
      const char *uri;
    } address;
    struct {
      esp_err_t (*crt_bundle_attach)(void *);
    } verification;
  } broker;
  struct {
    const char *username, *client_id;
    struct {
      const char *password;
    } authentication;
  } credentials;
  struct {
    int protocol_ver, keepalive;
  } session;
  struct {
    int timeout_ms;
    bool disable_auto_reconnect;
  } network;
  struct {
    int size;
  } buffer;
  struct {
    int stack_size;
  } task;
  struct {
    int limit;
  } outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, int,
                                         void (*)(void *, esp_event_base_t,
                                                  int32_t, void *),
                                         void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *, int);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char *,
                            const char *, int, int, int, bool);
