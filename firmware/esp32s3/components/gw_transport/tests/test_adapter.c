/* Real MQTT/HTTPS adapter; the SDK event loop, socket and mutex clock are
 * mocked. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../gw_transport.c"
#include <assert.h>

static uint64_t now;
static bool trusted = true, fail_enqueue;
static const char *mode = "MQTT";
static unsigned http_calls, publishes, stops, starts, reconnects,
    subscription_calls;
static char published_topic[320], published_json[2048], http_interface[32];
static int mqtt_id = 10;
static uint64_t advance_on_request;
static bool drop_optional_on_request;
static const char *scheduled[3];
static unsigned schedule_at, delay_calls;
static int fail_malloc = -1;
typedef struct {
  bool held;
} mock_lock_t;

static void dispatch(int id, int message) {
  esp_mqtt_event_t e = {.msg_id = message};
  event(NULL, NULL, id, &e);
}
static void data_event(const char *topic, const char *bytes, size_t length,
                       int offset, int total_length, int message, int qos,
                       int retain) {
  esp_mqtt_event_t e = {.msg_id = message,
                        .topic = (char *)topic,
                        .topic_len = topic ? (int)strlen(topic) : 0,
                        .data = (char *)bytes,
                        .data_len = (int)length,
                        .current_data_offset = offset,
                        .total_data_len = total_length,
                        .qos = qos,
                        .retain = retain};
  event(NULL, NULL, MQTT_EVENT_DATA, &e);
}
static void message(const char *suffix, const char *json, int qos, int retain) {
  char topic[320];
  snprintf(topic, sizeof(topic), "%s/%s", prefix, suffix);
  data_event(topic, json, strlen(json), 0, (int)strlen(json), 1, qos, retain);
}
uint64_t gw_core_mono_ms(void) { return now; }
bool gw_core_time_trusted(void) { return trusted; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  return calloc(1, sizeof(mock_lock_t));
}
int xSemaphoreTake(SemaphoreHandle_t h, uint64_t ticks) {
  (void)ticks;
  mock_lock_t *m = h;
  assert(m);
  if (m->held)
    return 0;
  m->held = true;
  if (h == request_lock && advance_on_request) {
    now = advance_on_request;
    advance_on_request = 0;
  }
  if (h == request_lock && drop_optional_on_request) {
    subscribed = 7;
    drop_optional_on_request = false;
  }
  return 1;
}
void xSemaphoreGive(SemaphoreHandle_t h) {
  mock_lock_t *m = h;
  assert(m && m->held);
  m->held = false;
}
void vTaskDelay(unsigned milliseconds) {
  now += milliseconds;
  delay_calls++;
  if (schedule_at < 3 && scheduled[schedule_at]) {
    const char *json = scheduled[schedule_at++];
    message("telemetry-ack", json, 1, 0);
  }
}
esp_err_t gw_factory_get_string(const char *key, char *out, size_t capacity) {
  const char *s = !strcmp(key, "cloud_link") ? mode
                  : !strcmp(key, "mqtt_url")
                      ? "mqtts://broker.example.test:8883"
                  : !strcmp(key, "mqtt_pass") ? "test-secret"
                                              : NULL;
  if (!s)
    return ESP_FAIL;
  if (strlen(s) >= capacity)
    return ESP_ERR_INVALID_SIZE;
  strcpy(out, s);
  return ESP_OK;
}
esp_err_t gw_http_json(const char *path, const char *method, const cJSON *body,
                       const char *interface, uint64_t deadline, cJSON **out) {
  (void)path;
  (void)method;
  (void)body;
  assert(deadline >= now);
  http_calls++;
  snprintf(http_interface, sizeof(http_interface), "%s",
           interface ? interface : "");
  *out = cJSON_CreateObject();
  return trusted ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t gw_http_tls_prepare(void) {
  return trusted ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t gw_http_tls_attach(void *config) {
  (void)config;
  return ESP_OK;
}
esp_mqtt_client_handle_t
esp_mqtt_client_init(const esp_mqtt_client_config_t *c) {
  assert(!strncmp(c->broker.address.uri, "mqtts://", 8));
  assert(c->broker.verification.crt_bundle_attach == gw_http_tls_attach);
  assert(c->network.disable_auto_reconnect &&
         c->outbox.limit == MESSAGE_LIMIT * 2);
  return (void *)99;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int id,
                                         void (*fn)(void *, esp_event_base_t,
                                                    int32_t, void *),
                                         void *context) {
  (void)c;
  (void)id;
  (void)context;
  assert(fn == event);
  return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
  assert(c);
  starts++;
  return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  assert(c);
  stops++;
  dispatch(MQTT_EVENT_DISCONNECTED, 0);
  return ESP_OK;
}
esp_err_t esp_mqtt_client_reconnect(esp_mqtt_client_handle_t c) {
  assert(c);
  reconnects++;
  return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic,
                              int qos) {
  assert(c && qos == 1 && !strncmp(topic, prefix, strlen(prefix)));
  subscription_calls++;
  return ++mqtt_id;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic,
                            const char *data, int length, int qos, int retain,
                            bool store) {
  assert(c && qos == 1 && !retain && store && length == (int)strlen(data));
  publishes++;
  snprintf(published_topic, sizeof(published_topic), "%s", topic);
  snprintf(published_json, sizeof(published_json), "%s", data);
  dispatch(MQTT_EVENT_PUBLISHED, 777);
  return fail_enqueue ? -1 : 777;
}
static void *allocation(size_t size) {
  if (fail_malloc == 0)
    return NULL;
  if (fail_malloc > 0)
    fail_malloc--;
  return malloc(size);
}
static void reset(const char *choice) {
  cJSON_InitHooks(NULL);
  free(lock);
  free(request_lock);
  lock = request_lock = NULL;
  cJSON_Delete(commands);
  commands = NULL;
  cJSON_Delete(reply);
  reply = NULL;
  reset_fragment();
  client = NULL;
  configured = started = mqtt_only = false;
  connected = ready = false;
  subscribed = 0;
  memset(subscriptions, 0, sizeof(subscriptions));
  expected_ack[0] = 0;
  mode = choice;
  now = 1000;
  trusted = true;
  http_calls = publishes = stops = starts = reconnects = subscription_calls = 0;
  advance_on_request = 0;
  drop_optional_on_request = false;
  fail_enqueue = false;
  fail_malloc = -1;
  memset(scheduled, 0, sizeof(scheduled));
  schedule_at = delay_calls = 0;
  assert(gw_transport_init("n", "g") == ESP_OK);
}
static void online(void) {
  gw_transport_service();
  assert(started && starts == 1);
  dispatch(MQTT_EVENT_CONNECTED, 0);
  assert(connected && !ready && subscription_calls == 4);
  for (unsigned i = 0; i < 4; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  assert(ready);
}
static cJSON *batch(void) {
  return cJSON_Parse(
      "{\"schemaVersion\":\"1.1\",\"namespaceId\":\"n\",\"gatewayId\":\"g\","
      "\"batchId\":\"stable\",\"events\":[]}");
}
static const char command[] =
    "{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"taskId\":\"t\"}";
static const char good_ack[] =
    "{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"batchId\":\"stable\","
    "\"durability\":\"RAW_PERSISTED\"}";
static const char old_ack[] =
    "{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"batchId\":\"old\","
    "\"durability\":\"RAW_PERSISTED\"}";

static void test_application_ack(void) {
  reset("MQTT");
  online();
  cJSON *b = batch(), *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b, NULL,
                           now + 30, &out) == ESP_ERR_TIMEOUT &&
         !out);
  assert(
      publishes == 1 &&
      !strcmp(published_topic,
              "gw/v1/n/g/telemetry")); /* PUBACK was delivered synchronously */
  scheduled[0] = old_ack;
  scheduled[1] = good_ack;
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b, NULL,
                           now + 100, &out) == ESP_OK &&
         out);
  assert(delay_calls >= 3 &&
         !strcmp(cJSON_GetObjectItem(out, "batchId")->valuestring, "stable"));
  cJSON_Delete(out);
  fail_enqueue = true;
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b, NULL,
                           now + 100, &out) == ESP_FAIL &&
         !out);
  assert(!expected_ack[0] && !reply);
  cJSON_Delete(b);
}
static void test_transport_selection_and_time(void) {
  reset("AUTO");
  cJSON *b = batch(), *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b, NULL,
                           now + 100, &out) == ESP_OK &&
         http_calls == 1 && !publishes);
  cJSON_Delete(out);
  online();
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b,
                           "WIFI_STA", now + 100, &out) == ESP_OK &&
         http_calls == 2 && !strcmp(http_interface, "WIFI_STA"));
  cJSON_Delete(out);
  assert(gw_transport_json("/api/v1/gateway-network-state", "PUT", b, NULL,
                           now + 100, &out) == ESP_OK &&
         http_calls == 3);
  cJSON_Delete(out);
  trusted = false;
  gw_transport_service();
  assert(stops == 1 && !started && !connected && !ready);
  trusted = true;
  gw_transport_service();
  assert(starts == 2);
  cJSON_Delete(b);
}
static void test_command_frames(void) {
  reset("MQTT");
  online();
  message("commands", command, 1, 1);
  message("commands", command, 0, 0);
  assert(!cJSON_GetArraySize(commands));
  char topic[320];
  snprintf(topic, sizeof(topic), "%s/commands", prefix);
  size_t split = 15;
  data_event(topic, command, split, 0, sizeof(command) - 1, 4, 1, 0);
  assert(fragment);
  data_event(NULL, command + split, sizeof(command) - 1 - split, (int)split,
             sizeof(command) - 1, 4, 1, 0);
  assert(!fragment && cJSON_GetArraySize(commands) == 1);
  cJSON *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-ingress/commands", "GET", NULL,
                           NULL, now + 100, &out) == ESP_OK);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(out, "commands")) == 1 &&
         !cJSON_GetArraySize(commands));
  cJSON_Delete(out);
  data_event(topic, command, split, 0, sizeof(command) - 1, 5, 1, 0);
  now += 5000;
  data_event(NULL, command + split, sizeof(command) - 1 - split, (int)split,
             sizeof(command) - 1, 5, 1, 0);
  assert(!fragment && !cJSON_GetArraySize(commands));
  for (unsigned i = 0; i < 17; i++)
    message("commands", command, 1, 0);
  assert(cJSON_GetArraySize(commands) == 16);
}
static void test_stale_event_and_budget(void) {
  reset("MQTT");
  online();
  dispatch(MQTT_EVENT_DISCONNECTED, 0);
  assert(!ready);
  for (unsigned i = 0; i < 4; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  assert(!ready);
  dispatch(MQTT_EVENT_CONNECTED, 0);
  for (unsigned i = 0; i < 4; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  assert(ready);
  cJSON *b = batch(), *out = NULL;
  advance_on_request = now + 100;
  assert(gw_transport_json("/api/v1/gateway-ingress/batches", "POST", b, NULL,
                           now + 100, &out) == ESP_ERR_TIMEOUT &&
         !publishes);
  cJSON_Delete(b);
}
static void test_fragment_identity(void) {
  reset("MQTT");
  online();
  char topic[320];
  snprintf(topic, sizeof(topic), "%s/commands", prefix);
  data_event(topic, command, 10, 0, sizeof(command) - 1, 5, 1, 0);
  data_event(NULL, command + 10, sizeof(command) - 11, 10, sizeof(command) - 1,
             6, 1, 0);
  assert(!fragment && !cJSON_GetArraySize(commands));
}
static void test_command_oom(void) {
  reset("MQTT");
  online();
  message("commands", command, 1, 0);
  cJSON_Hooks hooks = {.malloc_fn = allocation, .free_fn = free};
  cJSON_InitHooks(&hooks);
  fail_malloc = 1;
  cJSON *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-ingress/commands", "GET", NULL,
                           NULL, now + 100, &out) == ESP_ERR_NO_MEM);
  cJSON_InitHooks(NULL);
  assert(cJSON_GetArraySize(commands) == 1);
  cJSON_Delete(out);
}
static void test_subscription_and_json_rejection(void) {
  reset("MQTT");
  gw_transport_service();
  dispatch(MQTT_EVENT_CONNECTED, 0);
  for (unsigned i = 0; i < 3; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  char refusal = (char)128;
  esp_mqtt_event_t denied = {
      .msg_id = subscriptions[3], .data = &refusal, .data_len = 1};
  event(NULL, NULL, MQTT_EVENT_SUBSCRIBED, &denied);
  assert(!(subscribed & 8));
  dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[3]);
  assert(ready);
  const char duplicate[] =
      "{\"namespaceId\":\"n\",\"namespaceId\":\"n\",\"gatewayId\":\"g\"}";
  message("commands", duplicate, 1, 0);
  assert(!cJSON_GetArraySize(commands));
  char deep[128];
  memset(deep, '[', 17);
  memset(deep + 17, ']', 17);
  deep[34] = 0;
  message("commands", deep, 1, 0);
  assert(!fragment && !cJSON_GetArraySize(commands));
  message("commands", command, 1, 0);
  advance_on_request = now + 100;
  cJSON *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-ingress/commands", "GET", NULL,
                           NULL, now + 100, &out) == ESP_ERR_TIMEOUT);
  assert(cJSON_GetArraySize(commands) == 1 && !out);
}
static void test_optional_channel(void) {
  reset("MQTT");
  gw_transport_service();
  dispatch(MQTT_EVENT_CONNECTED, 0);
  for (unsigned i = 0; i < 3; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  assert(ready);
  cJSON *b = batch(), *out = NULL;
  assert(gw_transport_json("/api/v1/gateway-peripheral/batches", "POST", b,
                           NULL, now + 100, &out) == ESP_ERR_INVALID_STATE &&
         !publishes);
  dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[3]);
  drop_optional_on_request = true;
  assert(gw_transport_json("/api/v1/gateway-peripheral/batches", "POST", b,
                           NULL, now + 100, &out) != ESP_OK &&
         !publishes);
  cJSON_Delete(b);
  reset("AUTO");
  gw_transport_service();
  dispatch(MQTT_EVENT_CONNECTED, 0);
  for (unsigned i = 0; i < 3; i++)
    dispatch(MQTT_EVENT_SUBSCRIBED, subscriptions[i]);
  assert(ready);
  b = batch();
  assert(gw_transport_json("/api/v1/gateway-peripheral/batches", "POST", b,
                           NULL, now + 100, &out) == ESP_OK &&
         http_calls == 1 && !publishes);
  cJSON_Delete(out);
  cJSON_Delete(b);
}
int main(void) {
  test_application_ack();
  test_transport_selection_and_time();
  test_command_frames();
  test_stale_event_and_budget();
  test_fragment_identity();
  test_command_oom();
  test_subscription_and_json_rejection();
  test_optional_channel();
  puts("PASS: production MQTT adapter "
       "event/subscription/deadline/fragment/ACK/HTTPS-bound fallback and "
       "command OOM scenarios");
  return 0;
}
