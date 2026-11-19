#include "gw_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_transport_rules.h"
#include "mqtt_client.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MESSAGE_LIMIT (128u * 1024u)
static esp_mqtt_client_handle_t client;
static SemaphoreHandle_t lock, request_lock;
static char ns[129], gw[129], prefix[280], uri[512], password[512];
static bool configured, started, mqtt_only;
static atomic_bool connected, ready;
static int subscriptions[4];
static atomic_uint subscribed;
static int fragment_id;
static char *fragment;
static size_t total, used;
static uint64_t fragment_deadline;
static char incoming_topic[320], expected_ack[320];
static cJSON *commands, *reply;
static const char *const suffixes[] = {"commands", "telemetry-ack",
                                       "result-ack", "peripheral-ack"};
static void reset_fragment(void) {
  free(fragment);
  fragment = NULL;
  used = total = 0;
  incoming_topic[0] = 0;
}
static void event(void *ctx, esp_event_base_t base, int32_t id, void *data) {
  (void)ctx;
  (void)base;
  esp_mqtt_event_handle_t e = data;
  if (id == MQTT_EVENT_CONNECTED) {
    connected = true;
    ready = false;
    subscribed = 0;
    for (unsigned i = 0; i < 4; i++) {
      char topic[320];
      snprintf(topic, sizeof(topic), "%s/%s", prefix, suffixes[i]);
      subscriptions[i] = esp_mqtt_client_subscribe(client, topic, 1);
    }
    return;
  }
  if (id == MQTT_EVENT_DISCONNECTED) {
    connected = false;
    ready = false;
    reset_fragment();
    return;
  }
  if (id == MQTT_EVENT_SUBSCRIBED) {
    if (!connected || e->msg_id <= 0)
      return;
    if (e->data && e->data_len > 0)
      for (int i = 0; i < e->data_len; i++)
        if ((unsigned char)e->data[i] >= 128)
          return;
    for (unsigned i = 0; i < 4; i++)
      if (subscriptions[i] > 0 && e->msg_id == subscriptions[i])
        subscribed |= 1u << i;
    ready = (subscribed & 7u) == 7u;
    return;
  }
  /* PUBACK intentionally has no journal/spool consequence. */
  if (id != MQTT_EVENT_DATA)
    return;
  if (!gw_core_time_trusted() || e->retain || e->qos != 1) {
    reset_fragment();
    return;
  }
  if (e->current_data_offset == 0) {
    reset_fragment();
    if (e->total_data_len < 1 || (unsigned)e->total_data_len > MESSAGE_LIMIT ||
        e->topic_len < 1 || e->topic_len >= (int)sizeof(incoming_topic))
      return;
    memcpy(incoming_topic, e->topic, e->topic_len);
    incoming_topic[e->topic_len] = 0;
    bool known = false;
    for (unsigned i = 0; i < 4; i++) {
      char topic[320];
      snprintf(topic, sizeof(topic), "%s/%s", prefix, suffixes[i]);
      if (!strcmp(topic, incoming_topic))
        known = true;
    }
    if (!known) {
      reset_fragment();
      return;
    }
    total = e->total_data_len;
    fragment_id = e->msg_id;
    fragment = malloc(total + 1);
    fragment_deadline = gw_core_mono_ms() + 5000;
    if (!fragment) {
      reset_fragment();
      return;
    }
  }
  if (!fragment || gw_core_mono_ms() >= fragment_deadline ||
      e->current_data_offset < 0 || (size_t)e->current_data_offset != used ||
      e->data_len < 0 || (size_t)e->data_len > total - used ||
      e->total_data_len != (int)total || e->msg_id != fragment_id ||
      (e->current_data_offset && e->topic_len &&
       (e->topic_len != (int)strlen(incoming_topic) ||
        memcmp(e->topic, incoming_topic, e->topic_len)))) {
    reset_fragment();
    return;
  }
  memcpy(fragment + used, e->data, e->data_len);
  used += e->data_len;
  if (used != total)
    return;
  fragment[used] = 0;
  cJSON *j = gw_transport_parse(fragment, used);
  if (gw_transport_identity(j, ns, gw)) {
    xSemaphoreTake(lock, portMAX_DELAY);
    char command_topic[320];
    snprintf(command_topic, sizeof(command_topic), "%s/commands", prefix);
    if (!strcmp(incoming_topic, command_topic)) {
      if (cJSON_GetArraySize(commands) < 16) {
        cJSON_AddItemToArray(commands, j);
        j = NULL;
      }
    } else if (expected_ack[0] && !strcmp(expected_ack, incoming_topic) &&
               !reply) {
      reply = j;
      j = NULL;
    }
    xSemaphoreGive(lock);
  }
  cJSON_Delete(j);
  reset_fragment();
}
esp_err_t gw_transport_init(const char *namespace_id, const char *gateway_id) {
  if (!namespace_id || !gateway_id || strlen(namespace_id) > 128 ||
      strlen(gateway_id) > 128)
    return ESP_ERR_INVALID_ARG;
  snprintf(ns, sizeof(ns), "%s", namespace_id);
  snprintf(gw, sizeof(gw), "%s", gateway_id);
  snprintf(prefix, sizeof(prefix), "gw/v1/%s/%s", ns, gw);
  char mode[16];
  if (gw_factory_get_string("cloud_link", mode, sizeof(mode)) != ESP_OK ||
      !strcmp(mode, "HTTPS"))
    return ESP_OK;
  if (strcmp(mode, "MQTT") && strcmp(mode, "AUTO"))
    return ESP_ERR_INVALID_ARG;
  mqtt_only = !strcmp(mode, "MQTT");
  if (gw_factory_get_string("mqtt_url", uri, sizeof(uri)) != ESP_OK ||
      strncmp(uri, "mqtts://", 8) || strchr(uri + 8, '@') ||
      strchr(uri + 8, '?') || strchr(uri + 8, '#') ||
      gw_factory_get_string("mqtt_pass", password, sizeof(password)) !=
          ESP_OK ||
      !password[0])
    return ESP_ERR_INVALID_STATE;
  lock = xSemaphoreCreateMutex();
  request_lock = xSemaphoreCreateMutex();
  commands = cJSON_CreateArray();
  if (!lock || !request_lock || !commands)
    return ESP_ERR_NO_MEM;
  configured = true;
  return ESP_OK;
}
void gw_transport_service(void) {
  if (!configured)
    return;
  if (!gw_core_time_trusted()) {
    ready = false;
    if (started) {
      esp_mqtt_client_stop(client);
      started = false;
      connected = false;
    }
    return;
  }
  if (!client) {
    if (gw_http_tls_prepare() != ESP_OK)
      return;
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = gw_http_tls_attach,
        .credentials.username = gw,
        .credentials.client_id = prefix,
        .credentials.authentication.password = password,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 30,
        .network.timeout_ms = 2000,
        .network.disable_auto_reconnect = true,
        .buffer.size = 2048,
        .task.stack_size = 8192,
        .outbox.limit = MESSAGE_LIMIT * 2};
    client = esp_mqtt_client_init(&cfg);
    if (!client)
      return;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, event, NULL);
  }
  if (!started) {
    if (esp_mqtt_client_start(client) == ESP_OK)
      started = true;
  } else if (!connected)
    esp_mqtt_client_reconnect(client);
}
esp_err_t gw_transport_json(const char *path, const char *method,
                            const cJSON *body, const char *interface,
                            uint64_t deadline, cJSON **out) {
  if (!out || !path || !method)
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  const char *ack_suffix, *suffix = gw_transport_topic(path, &ack_suffix);
  bool channel_ready =
      ready &&
      (!suffix || strcmp(suffix, "peripheral-events") || (subscribed & 8u));
  if (interface || !configured || !suffix || (!channel_ready && !mqtt_only))
    return gw_http_json(path, method, body, interface, deadline, out);
  if (!channel_ready || !gw_core_time_trusted())
    return ESP_ERR_INVALID_STATE;
  uint64_t now = gw_core_mono_ms();
  if (now >= deadline)
    return ESP_ERR_TIMEOUT;
  if (!xSemaphoreTake(request_lock, pdMS_TO_TICKS(deadline - now)))
    return ESP_ERR_TIMEOUT;
  esp_err_t result = ESP_ERR_TIMEOUT;
  if (gw_core_mono_ms() >= deadline || !ready || !gw_core_time_trusted() ||
      (!strcmp(suffix, "peripheral-events") && !(subscribed & 8u)))
    goto done;
  if (!strcmp(suffix, "commands")) {
    if (strcmp(method, "GET")) {
      result = ESP_ERR_INVALID_ARG;
      goto done;
    }
    cJSON *response = cJSON_CreateObject(), *replacement = cJSON_CreateArray();
    if (!response || !replacement) {
      cJSON_Delete(response);
      cJSON_Delete(replacement);
      result = ESP_ERR_NO_MEM;
      goto done;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!cJSON_AddItemToObject(response, "commands", commands)) {
      xSemaphoreGive(lock);
      cJSON_Delete(response);
      cJSON_Delete(replacement);
      result = ESP_ERR_NO_MEM;
      goto done;
    }
    commands = replacement;
    *out = response;
    xSemaphoreGive(lock);
    result = ESP_OK;
    goto done;
  }
  if (strcmp(method, "POST") || !gw_transport_identity(body, ns, gw)) {
    result = ESP_ERR_INVALID_ARG;
    goto done;
  }
  char topic[320];
  snprintf(topic, sizeof(topic), "%s/%s", prefix, suffix);
  char *raw = cJSON_PrintUnformatted(body);
  if (!raw || strlen(raw) > MESSAGE_LIMIT) {
    free(raw);
    result = ESP_ERR_INVALID_SIZE;
    goto done;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  cJSON_Delete(reply);
  reply = NULL;
  expected_ack[0] = 0;
  if (ack_suffix)
    snprintf(expected_ack, sizeof(expected_ack), "%s/%s", prefix, ack_suffix);
  xSemaphoreGive(lock);
  int published =
      esp_mqtt_client_enqueue(client, topic, raw, strlen(raw), 1, 0, true);
  free(raw);
  if (published < 0) {
    result = ESP_FAIL;
    goto done;
  }
  if (!ack_suffix) {
    *out = cJSON_CreateObject();
    result = *out ? ESP_OK : ESP_ERR_NO_MEM;
    goto done;
  }
  while (ready && gw_core_time_trusted() && gw_core_mono_ms() < deadline) {
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON *candidate = reply;
    reply = NULL;
    xSemaphoreGive(lock);
    if (candidate) {
      if (gw_transport_ack_matches(candidate, body, path)) {
        *out = candidate;
        result = ESP_OK;
        break;
      }
      cJSON_Delete(candidate);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
done:
  xSemaphoreTake(lock, portMAX_DELAY);
  expected_ack[0] = 0;
  cJSON_Delete(reply);
  reply = NULL;
  xSemaphoreGive(lock);
  xSemaphoreGive(request_lock);
  return result;
}
