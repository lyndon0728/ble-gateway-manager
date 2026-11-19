/* nRF Central executor. UART acknowledgement means acceptance, not completion.
 * Each op_seq and exact request is durable before the first UART transmission.
 * Recovery only queries facts; it never repeats a physical ATT write. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_capture.h"
#include "gw_core.h"
#include "gw_nrf.h"
#include "mbedtls/base64.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_KEY "gatt/nrf-operation"
#define CONN_KEY "gatt/nrf-connection"
#define RESULT_KEY "gatt/nrf-result"
static SemaphoreHandle_t lock;
static bool recovering = true, storage_fault;
static const char *str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static void string(cJSON *o, const char *key, const char *v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, key);
  cJSON_AddStringToObject(o, key, v);
}
static void number(cJSON *o, const char *key, double v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, key);
  cJSON_AddNumberToObject(o, key, v);
}
static bool integer(const cJSON *o, const char *key, uint32_t min, uint32_t max,
                    uint32_t *n) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < min ||
      v->valuedouble > max || floor(v->valuedouble) != v->valuedouble)
    return false;
  *n = (uint32_t)v->valuedouble;
  return true;
}
static bool decimal(const char *s, uint64_t *v) {
  if (!s || !*s || (*s == '0' && s[1]))
    return false;
  uint64_t n = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || n > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
      return false;
    n = n * 10 + (unsigned)(*s - '0');
  }
  *v = n;
  return true;
}
static void u64(cJSON *o, const char *key, uint64_t value) {
  char s[24];
  snprintf(s, sizeof(s), "%" PRIu64, value);
  string(o, key, s);
}
static int nibble(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static void hex(const uint8_t *in, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 15];
  }
  out[n * 2] = 0;
}
static bool unhex(const char *in, uint8_t *out, size_t n) {
  if (strlen(in) != n * 2)
    return false;
  for (size_t i = 0; i < n; i++) {
    int a = nibble(in[2 * i]), b = nibble(in[2 * i + 1]);
    if (a < 0 || b < 0)
      return false;
    out[i] = (uint8_t)(a * 16 + b);
  }
  return true;
}
static bool address(const char *in, uint8_t out[6]) {
  char compact[13];
  size_t n = strlen(in);
  if (n == 12)
    return unhex(in, out, 6);
  if (n != 17)
    return false;
  for (unsigned i = 0; i < 6; i++) {
    if (i < 5 && in[i * 3 + 2] != ':')
      return false;
    compact[i * 2] = in[i * 3];
    compact[i * 2 + 1] = in[i * 3 + 1];
  }
  compact[12] = 0;
  return unhex(compact, out, 6);
}
static bool uuid(const char *in, uint8_t out[16], char canonical[37]) {
  char full[37], compact[33];
  size_t n = strlen(in);
  if (n == 4)
    snprintf(full, sizeof(full), "0000%s-0000-1000-8000-00805f9b34fb", in);
  else if (n == 8)
    snprintf(full, sizeof(full), "%s-0000-1000-8000-00805f9b34fb", in);
  else if (n == 36)
    memcpy(full, in, 37);
  else
    return false;
  unsigned j = 0;
  for (unsigned i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (full[i] != '-')
        return false;
    } else
      compact[j++] = full[i];
  }
  compact[32] = 0;
  if (!unhex(compact, out, 16))
    return false;
  hex(out, 16, compact);
  j = 0;
  for (unsigned i = 0; i < 36; i++)
    canonical[i] =
        (i == 8 || i == 13 || i == 18 || i == 23) ? '-' : compact[j++];
  canonical[36] = 0;
  return true;
}
static esp_err_t persist(const char *key, const cJSON *value) {
  esp_err_t e = gw_store_put_json(key, value);
  if (e != ESP_OK)
    storage_fault = true;
  return e;
}
static esp_err_t fail(cJSON *details, const char *code, esp_err_t err) {
  string(details, "code", code);
  return err;
}
static esp_err_t final_result(const char *task_id, esp_err_t result,
                              const cJSON *details, bool publish_connection) {
  cJSON *j = cJSON_CreateObject();
  if (!j)
    return ESP_ERR_NO_MEM;
  string(j, "taskId", task_id);
  number(j, "result", result);
  cJSON_AddItemToObject(j, "details", cJSON_Duplicate(details, true));
  char *raw = cJSON_PrintUnformatted(j),
       *conn = publish_connection ? cJSON_PrintUnformatted(details) : NULL;
  cJSON_Delete(j);
  if (!raw || (publish_connection && !conn)) {
    free(raw);
    free(conn);
    return ESP_ERR_NO_MEM;
  }
  gw_store_op_t ops[2] = {
      {.key = RESULT_KEY, .data = raw, .size = strlen(raw)},
      {.key = CONN_KEY, .data = conn, .size = conn ? strlen(conn) : 0}};
  esp_err_t e = gw_store_transaction(ops, publish_connection ? 2 : 1);
  free(raw);
  free(conn);
  if (e != ESP_OK)
    storage_fault = true;
  return e;
}
static void base64(cJSON *o, const char *key, const uint8_t *p, size_t n) {
  unsigned char encoded[689];
  size_t size = 0;
  if (mbedtls_base64_encode(encoded, sizeof(encoded), &size, p, n) == 0) {
    encoded[size] = 0;
    string(o, key, (const char *)encoded);
  }
}
static bool current_boot(const uint8_t expected[16]) {
  uint8_t actual[16];
  return gw_nrf_identity(actual, NULL, NULL) && !memcmp(actual, expected, 16);
}
static esp_err_t query(uint8_t opcode, const void *args, size_t n,
                       uint8_t *result, size_t *size, uint64_t deadline,
                       uint8_t *peer_status) {
  uint8_t status = 255;
  esp_err_t e =
      gw_nrf_command(opcode, args, n, &status, result, size, (int64_t)deadline);
  if (peer_status)
    *peer_status = status;
  if (e == ESP_OK && status)
    e = status == 7   ? ESP_ERR_NOT_FOUND
        : status == 2 ? ESP_ERR_NOT_SUPPORTED
                      : ESP_FAIL;
  return e;
}

/* Query one durable intent, preserving an unresolved result across all resets.
 * NOT_FOUND is not permission to repeat the original operation: it can be the
 * post-journal/pre-send crash window, or loss of the controller's evidence. */
static esp_err_t resolve(cJSON *journal, uint64_t deadline, uint8_t *out,
                         size_t *out_size) {
  uint8_t boot[16], args[8], reply[528];
  uint64_t seq = 0;
  uint32_t opcode = 0;
  if (!unhex(str(journal, "controllerBootId"), boot, 16) ||
      !decimal(str(journal, "sequence"), &seq) || !seq ||
      !integer(journal, "opcode", 0x10, 0x18, &opcode))
    return ESP_ERR_INVALID_STATE;
  if (!current_boot(boot)) {
    string(journal, "phase", "BOOT_CHANGED");
    persist(OP_KEY, journal);
    return ESP_ERR_TIMEOUT;
  }
  gw_put_u64(args, seq);
  do {
    size_t size = sizeof(reply);
    esp_err_t e = query(0x17, args, sizeof(args), reply, &size, deadline, NULL);
    if (e != ESP_OK || size < 12 || gw_get_u64(reply) != seq ||
        reply[8] != opcode || reply[9] > 4) {
      string(journal, "phase", "UNKNOWN");
      persist(OP_KEY, journal);
      return ESP_ERR_TIMEOUT;
    }
    if (reply[9]) {
      char encoded[sizeof(reply) * 2 + 1];
      hex(reply, size, encoded);
      string(journal, "resultHex", encoded);
      number(journal, "nrfState", reply[9]);
      number(journal, "error", reply[10]);
      number(journal, "attError", reply[11]);
      string(journal, "phase", reply[9] == 4 ? "UNKNOWN" : "TERMINAL");
      if (persist(OP_KEY, journal) != ESP_OK)
        return ESP_ERR_TIMEOUT;
      if (reply[9] == 4)
        return ESP_ERR_TIMEOUT;
      if (reply[9] != 1)
        return ESP_FAIL;
      if (out && out_size) {
        if (size - 12 > *out_size)
          return ESP_ERR_INVALID_SIZE;
        memcpy(out, reply + 12, size - 12);
        *out_size = size - 12;
      }
      return ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  } while (gw_core_mono_ms() < deadline && current_boot(boot));
  string(journal, "phase", "UNKNOWN");
  persist(OP_KEY, journal);
  return ESP_ERR_TIMEOUT;
}

static esp_err_t operation(const char *task_id, const uint8_t boot[16],
                           uint8_t opcode, const uint8_t *args, size_t n,
                           uint64_t deadline, uint8_t *out, size_t *size) {
  if (storage_fault)
    return ESP_ERR_INVALID_STATE;
  uint64_t now = gw_core_mono_ms();
  if (deadline <= now || deadline - now < 100 || n > 520)
    return ESP_ERR_TIMEOUT;
  cJSON *old = gw_store_get_json(OP_KEY);
  uint64_t seq = 0;
  if (old) {
    uint8_t previous_boot[16];
    if (!unhex(str(old, "controllerBootId"), previous_boot, 16) ||
        !decimal(str(old, "sequence"), &seq)) {
      cJSON_Delete(old);
      storage_fault = true;
      return ESP_ERR_INVALID_STATE;
    }
    if (!memcmp(previous_boot, boot, 16)) {
      const char *phase = str(old, "phase");
      if (strcmp(phase, "TERMINAL") && strcmp(phase, "REJECTED")) {
        uint8_t ignored[516];
        size_t ignored_size = sizeof(ignored);
        resolve(old, deadline, ignored, &ignored_size);
        if (strcmp(str(old, "phase"), "TERMINAL")) {
          cJSON_Delete(old);
          return ESP_ERR_TIMEOUT;
        }
      }
    } else
      seq = 0;
    cJSON_Delete(old);
  } else {
    size_t stored = 0;
    if (gw_store_get(OP_KEY, NULL, &stored) != ESP_ERR_NOT_FOUND) {
      storage_fault = true;
      return ESP_ERR_INVALID_STATE;
    }
  }
  if (seq == UINT64_MAX || !current_boot(boot))
    return ESP_ERR_INVALID_STATE;
  seq++;
  now = gw_core_mono_ms();
  if (now >= deadline)
    return ESP_ERR_TIMEOUT;
  uint8_t request[530], accepted[8];
  gw_put_u64(request, seq);
  uint64_t remaining = deadline - now;
  if (remaining > 30000)
    remaining = 30000;
  if (remaining < 100)
    return ESP_ERR_TIMEOUT;
  gw_put_u16(request + 8, (uint16_t)remaining);
  memcpy(request + 10, args, n);
  cJSON *j = cJSON_CreateObject();
  if (!j)
    return ESP_ERR_NO_MEM;
  char boot_hex[33], args_hex[1061];
  hex(boot, 16, boot_hex);
  hex(request, n + 10, args_hex);
  string(j, "controllerBootId", boot_hex);
  string(j, "taskId", task_id);
  u64(j, "sequence", seq);
  number(j, "opcode", opcode);
  string(j, "argsHex", args_hex);
  string(j, "phase", "INTENT");
  esp_err_t e = persist(OP_KEY, j);
  if (e != ESP_OK) {
    cJSON_Delete(j);
    return e;
  }
  if (!current_boot(boot)) {
    string(j, "phase", "BOOT_CHANGED");
    persist(OP_KEY, j);
    cJSON_Delete(j);
    return ESP_ERR_TIMEOUT;
  }
  uint8_t status = 255;
  size_t accepted_size = sizeof(accepted);
  e = query(opcode, request, n + 10, accepted, &accepted_size, deadline,
            &status);
  if (status != 255 && e != ESP_OK) {
    string(j, "phase", "REJECTED");
    number(j, "status", status);
    if (persist(OP_KEY, j) != ESP_OK)
      e = ESP_ERR_TIMEOUT;
  } else if (e != ESP_OK || accepted_size != 8 || gw_get_u64(accepted) != seq) {
    string(j, "phase", "UNKNOWN");
    persist(OP_KEY, j);
    e = ESP_ERR_TIMEOUT;
  } else
    e = resolve(j, deadline, out, size);
  cJSON_Delete(j);
  return e;
}

static esp_err_t info(const uint8_t boot[16], uint32_t id, uint8_t out[14],
                      uint64_t deadline) {
  uint8_t args[4];
  gw_put_u32(args, id);
  size_t n = 14;
  esp_err_t e = query(0x0a, args, 4, out, &n, deadline, NULL);
  if (e == ESP_OK &&
      (!current_boot(boot) || n != 14 || gw_get_u32(out) != id || out[4] > 1 ||
       gw_get_u16(out + 11) < 23 || gw_get_u16(out + 11) > 517 || out[13] > 2))
    return ESP_ERR_INVALID_RESPONSE;
  return e;
}
static cJSON *connection(const uint8_t boot[16], const uint8_t data[14],
                         const char *task_id) {
  cJSON *c = cJSON_CreateObject();
  if (!c)
    return NULL;
  char boot_hex[33], ref[33], addr[13];
  hex(boot, 16, boot_hex);
  hex(data + 5, 6, addr);
  for (unsigned i = 0; i < 12; i++)
    if (addr[i] >= 'a' && addr[i] <= 'f')
      addr[i] -= 'a' - 'A';
  gw_capture_connection_ref(0, boot, gw_get_u32(data), ref);
  string(c, "radio", "NRF54");
  string(c, "controllerBootId", boot_hex);
  number(c, "connectionId", gw_get_u32(data));
  string(c, "connectionRef", ref);
  string(c, "deviceAddress", addr);
  string(c, "addressType", data[4] ? "RANDOM" : "PUBLIC");
  number(c, "attMtu", gw_get_u16(data + 11));
  string(c, "securityState",
         data[13] == 2   ? "AUTHENTICATED_ENCRYPTED"
         : data[13] == 1 ? "ENCRYPTED"
                         : "UNENCRYPTED");
  string(c, "connectionState", "CONNECTED");
  string(c, "taskId", task_id);
  return c;
}
static esp_err_t publish_closed(cJSON *c, const char *reason) {
  uint8_t boot[16], addr[6];
  uint32_t id = 0;
  if (!unhex(str(c, "controllerBootId"), boot, 16) ||
      !address(str(c, "deviceAddress"), addr) ||
      !integer(c, "connectionId", 1, UINT32_MAX, &id))
    return ESP_ERR_INVALID_STATE;
  esp_err_t e = gw_capture_link_down(
      0, boot, id, !strcmp(str(c, "addressType"), "RANDOM"), addr, reason);
  if (e == ESP_OK) {
    string(c, "connectionState", "CLOSED");
    string(c, "linkReason", reason);
    e = persist(CONN_KEY, c);
  }
  return e;
}
static esp_err_t close_connection(const char *task_id, const uint8_t boot[16],
                                  uint32_t id, uint64_t deadline) {
  uint8_t args[4], result[14];
  gw_put_u32(args, id);
  size_t n = sizeof(result);
  esp_err_t e = operation(task_id, boot, 0x11, args, 4, deadline, result, &n);
  if (e != ESP_OK)
    return e;
  while (gw_core_mono_ms() < deadline) {
    e = info(boot, id, result, deadline);
    if (e == ESP_ERR_NOT_FOUND)
      return ESP_OK;
    if (e != ESP_OK)
      return e;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return ESP_ERR_TIMEOUT;
}
static bool peer_matches(const cJSON *params, const uint8_t data[14]) {
  uint8_t expected[6];
  const char *type = str(params, "addressType");
  if (!address(str(params, "deviceAddress"), expected) ||
      (strcmp(type, "PUBLIC") && strcmp(type, "RANDOM")))
    return false;
  return data[4] == (!strcmp(type, "RANDOM")) &&
         !memcmp(expected, data + 5, 6) &&
         (strcmp(str(params, "security"), "BONDED_ENCRYPTED") || data[13] > 0);
}

esp_err_t gw_gatt_nrf_execute(const cJSON *command, uint64_t deadline,
                              cJSON **details) {
  if (!lock || !details)
    return ESP_ERR_INVALID_ARG;
  *details = cJSON_CreateObject();
  if (!*details)
    return ESP_ERR_NO_MEM;
  cJSON *d = *details;
  string(d, "radio", "NRF54");
  if (xSemaphoreTake(lock, 0) != pdTRUE)
    return fail(d, "RADIO_BUSY", ESP_ERR_INVALID_STATE);
  esp_err_t e = ESP_ERR_INVALID_ARG;
  const char *op = str(command, "type"), *task_id = str(command, "taskId");
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(command, "parameters");
  uint8_t boot[16], reply[516], args[520], ci[14];
  uint32_t caps = 0, id = 0;
  size_t n = sizeof(reply);
  cJSON *saved = NULL;
  bool temporary = false, created = false;
  if (storage_fault) {
    e = fail(d, "STORAGE_FAULT", ESP_ERR_INVALID_STATE);
    goto done;
  }
  if (recovering) {
    e = fail(d, "RECOVERY_PENDING", ESP_ERR_INVALID_STATE);
    goto done;
  }
  if (!*task_id || strlen(task_id) > 128 || !cJSON_IsObject(p) ||
      (*str(p, "radio") && strcmp(str(p, "radio"), "NRF54"))) {
    e = fail(d, "INVALID_ARGUMENT", ESP_ERR_INVALID_ARG);
    goto done;
  }
  if (!gw_nrf_identity(boot, &caps, NULL) ||
      (caps & ((1u << 2) | (1u << 9))) != ((1u << 2) | (1u << 9))) {
    e = fail(d, "UNSUPPORTED", ESP_ERR_NOT_SUPPORTED);
    goto done;
  }
  uint32_t timeout = 0;
  if (!integer(p, "timeoutMs", 100, 30000, &timeout)) {
    e = fail(d, "INVALID_TIMEOUT", ESP_ERR_INVALID_ARG);
    goto done;
  }
  uint64_t budget = gw_core_mono_ms() + timeout;
  if (deadline > budget)
    deadline = budget;
  bool connect = !strcmp(op, "GATT_CONNECT"), read = !strcmp(op, "GATT_READ"),
       write = !strcmp(op, "GATT_WRITE"),
       disconnect = !strcmp(op, "GATT_DISCONNECT"),
       subscribe = !strcmp(op, "GATT_SUBSCRIBE"),
       unsubscribe = !strcmp(op, "GATT_UNSUBSCRIBE");
  if (!connect && !read && !write && !disconnect && !subscribe &&
      !unsubscribe) {
    e = fail(d, "UNSUPPORTED", ESP_ERR_NOT_SUPPORTED);
    goto done;
  }
  if ((subscribe || unsubscribe) && !(caps & (1u << 3))) {
    e = fail(d, "UNSUPPORTED", ESP_ERR_NOT_SUPPORTED);
    goto done;
  }
  if (write && ((strcmp(str(p, "writeMode"), "WITH_RESPONSE")) ||
                (strcmp(str(p, "verification"), "NONE") &&
                 strcmp(str(p, "verification"), "READBACK")))) {
    e = fail(d, "UNSUPPORTED_VERIFICATION", ESP_ERR_NOT_SUPPORTED);
    goto done;
  }
  const char *ref = str(p, "connectionRef");
  saved = gw_store_get_json(CONN_KEY);
  if (*ref) {
    uint8_t saved_boot[16];
    if (!saved || strcmp(str(saved, "connectionState"), "CONNECTED") ||
        strcmp(str(saved, "connectionRef"), ref) ||
        !unhex(str(saved, "controllerBootId"), saved_boot, 16) ||
        memcmp(boot, saved_boot, 16) ||
        !integer(saved, "connectionId", 1, UINT32_MAX, &id)) {
      e = fail(d, "STALE_CONNECTION", ESP_ERR_INVALID_STATE);
      goto done;
    }
    e = info(boot, id, ci, deadline);
    if (e != ESP_OK) {
      e = fail(d, "STALE_CONNECTION", e);
      goto done;
    }
    if ((read || write) && !peer_matches(p, ci)) {
      e = fail(d, "CONNECTION_PEER_MISMATCH", ESP_ERR_INVALID_ARG);
      goto done;
    }
  } else {
    if (!connect && !read && !write) {
      e = fail(d, "CONNECTION_REQUIRED", ESP_ERR_INVALID_ARG);
      goto done;
    }
    if (saved && !strcmp(str(saved, "connectionState"), "CONNECTED")) {
      e = fail(d, "RADIO_BUSY", ESP_ERR_INVALID_STATE);
      goto done;
    }
    uint8_t peer[6];
    const char *type = str(p, "addressType"), *security = str(p, "security");
    if (!address(str(p, "deviceAddress"), peer) ||
        (strcmp(type, "PUBLIC") && strcmp(type, "RANDOM")) ||
        (connect && strcmp(security, "UNENCRYPTED") &&
         strcmp(security, "BONDED_ENCRYPTED")) ||
        (*security && strcmp(security, "UNENCRYPTED") &&
         strcmp(security, "BONDED_ENCRYPTED"))) {
      e = fail(d, "INVALID_PEER", ESP_ERR_INVALID_ARG);
      goto done;
    }
    const cJSON *automatic = cJSON_GetObjectItemCaseSensitive(p, "autoTrigger");
    if (automatic) {
      uint32_t max_age = 0;
      if (!connect || !integer(automatic, "maxAgeMs", 1000, 30000, &max_age) ||
          !gw_capture_adv_evidence(str(automatic, "advEventId"),
                                   str(p, "deviceAddress"), type, boot,
                                   max_age)) {
        e = fail(d, "STALE_OBSERVATION", ESP_ERR_INVALID_STATE);
        goto done;
      }
    }
    cJSON_Delete(saved);
    saved = NULL;
    args[0] = !strcmp(type, "RANDOM");
    memcpy(args + 1, peer, 6);
    args[7] = !strcmp(security, "BONDED_ENCRYPTED");
    e = operation(task_id, boot, 0x10, args, 8, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n != 6 || !(id = gw_get_u32(reply))) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    created = true;
    temporary = !connect;
    e = info(boot, id, ci, deadline);
    if (e != ESP_OK || !peer_matches(p, ci)) {
      if (e == ESP_OK)
        e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    cJSON_Delete(saved);
    saved = connection(boot, ci, task_id);
    if (!saved) {
      e = ESP_ERR_NO_MEM;
      goto action_error;
    }
    /* Track every connection before further operations, including temporary
     * ones: recovery must close it even if the next Flash write is lost. */
    if (persist(CONN_KEY, saved) != ESP_OK) {
      e = ESP_ERR_TIMEOUT;
      goto action_error;
    }
  }
  cJSON_Delete(d);
  d = cJSON_Duplicate(saved, true);
  *details = d;
  if (!d) {
    e = ESP_ERR_NO_MEM;
    goto done;
  }
  if (disconnect) {
    e = close_connection(task_id, boot, id, deadline);
    if (e == ESP_OK)
      e = publish_closed(saved, "DISCONNECTED");
    if (e == ESP_OK) {
      string(d, "connectionState", "CLOSED");
      goto success;
    }
    goto action_error;
  }
  const cJSON *automatic = cJSON_GetObjectItemCaseSensitive(p, "autoTrigger");
  uint8_t service[16], characteristic[16];
  char service_text[37], char_text[37];
  uint16_t value = 0, ccc = 0;
  uint8_t properties = 0;
  if (connect && automatic) {
    if (!uuid(str(automatic, "serviceUuid"), service, service_text)) {
      e = ESP_ERR_INVALID_ARG;
      goto action_error;
    }
    gw_put_u32(args, id);
    memcpy(args + 4, service, 16);
    n = sizeof(reply);
    e = operation(task_id, boot, 0x18, args, 20, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n != 4 || !gw_get_u16(reply) ||
        gw_get_u16(reply) > gw_get_u16(reply + 2)) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    string(d, "serviceUuid", service_text);
    string(d, "ruleId", str(automatic, "ruleId"));
    string(d, "advEventId", str(automatic, "advEventId"));
    number(d, "serviceStartHandle", gw_get_u16(reply));
    number(d, "serviceEndHandle", gw_get_u16(reply + 2));
  }
  bool discover =
      !connect || (automatic && *str(automatic, "characteristicUuid"));
  if (discover) {
    const cJSON *source = connect ? automatic : p;
    if (!uuid(str(source, "serviceUuid"), service, service_text) ||
        !uuid(str(source, "characteristicUuid"), characteristic, char_text)) {
      e = ESP_ERR_INVALID_ARG;
      goto action_error;
    }
    gw_put_u32(args, id);
    memcpy(args + 4, service, 16);
    memcpy(args + 20, characteristic, 16);
    n = sizeof(reply);
    e = operation(task_id, boot, 0x12, args, 36, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n != 5 || !(value = gw_get_u16(reply))) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    ccc = gw_get_u16(reply + 2);
    properties = reply[4];
    string(d, "serviceUuid", service_text);
    string(d, "characteristicUuid", char_text);
    number(d, "characteristicHandle", value);
    number(d, "cccHandle", ccc);
  }
  if (connect) {
    e = info(boot, id, ci, deadline);
    if (e != ESP_OK || !peer_matches(p, ci)) {
      if (e == ESP_OK)
        e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    goto success;
  }
  gw_put_u32(args, id);
  gw_put_u16(args + 4, value);
  if (read) {
    if (!(properties & 0x02)) {
      e = ESP_ERR_NOT_SUPPORTED;
      goto action_error;
    }
    n = sizeof(reply);
    e = operation(task_id, boot, 0x13, args, 6, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n < 2 || gw_get_u16(reply) != n - 2) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    base64(d, "valueBase64", reply + 2, n - 2);
  } else if (write) {
    size_t length = 0;
    uint8_t value_bytes[512];
    const char *encoded = str(p, "valueBase64");
    uint32_t mtu = gw_get_u16(ci + 11);
    if (!(properties & 0x08) ||
        (*str(p, "verification") &&
         !strcmp(str(p, "verification"), "READBACK") && !(properties & 0x02))) {
      e = ESP_ERR_NOT_SUPPORTED;
      goto action_error;
    }
    if (mbedtls_base64_decode(value_bytes, sizeof(value_bytes), &length,
                              (const unsigned char *)encoded,
                              strlen(encoded)) ||
        !length || length > mtu - 3) {
      e = ESP_ERR_INVALID_SIZE;
      goto action_error;
    }
    gw_put_u16(args + 6, (uint16_t)length);
    memcpy(args + 8, value_bytes, length);
    n = sizeof(reply);
    e = operation(task_id, boot, 0x14, args, 8 + length, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    cJSON_AddTrueToObject(d, "attResponse");
    if (!strcmp(str(p, "verification"), "READBACK")) {
      n = sizeof(reply);
      e = operation(task_id, boot, 0x13, args, 6, deadline, reply, &n);
      if (e != ESP_OK)
        goto action_error;
      if (n != length + 2 || gw_get_u16(reply) != length ||
          memcmp(reply + 2, value_bytes, length)) {
        e = ESP_ERR_INVALID_RESPONSE;
        goto action_error;
      }
      base64(d, "readbackBase64", reply + 2, length);
    }
  } else if (subscribe || unsubscribe) {
    bool indicate = !strcmp(str(p, "mode"), "INDICATE");
    if (!ccc || (subscribe && strcmp(str(p, "mode"), "NOTIFY") && !indicate) ||
        (subscribe && !(properties & (indicate ? 0x20 : 0x10)))) {
      e = ESP_ERR_NOT_SUPPORTED;
      goto action_error;
    }
    gw_put_u16(args + 6, ccc);
    args[8] = indicate ? 2 : 1;
    n = sizeof(reply);
    e = operation(task_id, boot, subscribe ? 0x15 : 0x16, args,
                  subscribe ? 9 : 8, deadline, reply, &n);
    if (e != ESP_OK)
      goto action_error;
    if (n) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto action_error;
    }
    cJSON_AddBoolToObject(d, "cccEnabled", subscribe);
    if (subscribe)
      string(d, "mode", str(p, "mode"));
  }
  if (temporary) {
    e = close_connection(task_id, boot, id, deadline);
    if (e != ESP_OK)
      goto action_error;
    e = publish_closed(saved, "DISCONNECTED");
    if (e != ESP_OK)
      goto action_error;
    string(d, "connectionState", "CLOSED");
  }
success:
  string(d, "code", "OK");
  e = ESP_OK;
  goto done;
action_error:
  if (created && e != ESP_ERR_TIMEOUT && e != ESP_ERR_NOT_FINISHED &&
      current_boot(boot) && gw_core_mono_ms() + 100 < deadline) {
    esp_err_t closed = close_connection(task_id, boot, id, deadline);
    if (closed == ESP_OK && saved)
      closed = publish_closed(saved, "DISCONNECTED");
    if (closed != ESP_OK)
      e = ESP_ERR_TIMEOUT;
  }
  if (d)
    fail(d,
         e == ESP_ERR_TIMEOUT || e == ESP_ERR_NOT_FINISHED ? "GATT_UNKNOWN"
                                                           : "GATT_FAILED",
         e);
done:
  /* Final task evidence and a newly registered long-lived connection share
   * one durable snapshot. Recovery never reconstructs a successful write
   * from an acceptance ACK, a partial sequence, or a different task. */
  if (!storage_fault && *task_id && d &&
      final_result(task_id, e, d, e == ESP_OK && !strcmp(op, "GATT_CONNECT")) !=
          ESP_OK)
    e = ESP_ERR_TIMEOUT;
  cJSON_Delete(saved);
  xSemaphoreGive(lock);
  return e;
}

esp_err_t gw_gatt_nrf_recover(const cJSON *command, uint64_t deadline,
                              cJSON **details) {
  (void)deadline;
  if (!details || !lock)
    return ESP_ERR_INVALID_ARG;
  *details = NULL;
  if (xSemaphoreTake(lock, 0) != pdTRUE)
    return ESP_ERR_NOT_FINISHED;
  cJSON *j = gw_store_get_json(RESULT_KEY);
  esp_err_t e = ESP_ERR_TIMEOUT;
  const cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
  if (j && !strcmp(str(j, "taskId"), str(command, "taskId")) &&
      cJSON_IsNumber(result)) {
    *details =
        cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "details"), true);
    e = *details ? (esp_err_t)result->valueint : ESP_ERR_NO_MEM;
  }
  cJSON_Delete(j);
  xSemaphoreGive(lock);
  return e;
}

static void monitor(void *unused) {
  (void)unused;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (xSemaphoreTake(lock, 0) != pdTRUE)
      continue;
    uint8_t boot[16];
    uint32_t caps = 0;
    uint64_t deadline = gw_core_mono_ms() + 1500;
    if (storage_fault || !gw_nrf_identity(boot, &caps, NULL) ||
        (caps & ((1u << 2) | (1u << 9))) != ((1u << 2) | (1u << 9))) {
      xSemaphoreGive(lock);
      continue;
    }
    cJSON *j = gw_store_get_json(OP_KEY);
    if (j && strcmp(str(j, "phase"), "TERMINAL") &&
        strcmp(str(j, "phase"), "REJECTED") &&
        strcmp(str(j, "phase"), "BOOT_CHANGED")) {
      uint8_t ignored[516];
      size_t n = sizeof(ignored);
      resolve(j, deadline, ignored, &n);
    }
    bool unresolved = j && strcmp(str(j, "phase"), "TERMINAL") &&
                      strcmp(str(j, "phase"), "REJECTED") &&
                      strcmp(str(j, "phase"), "BOOT_CHANGED");
    cJSON_Delete(j);
    cJSON *c = gw_store_get_json(CONN_KEY);
    uint8_t previous[16], ci[14];
    uint32_t id = 0;
    if (c && !strcmp(str(c, "connectionState"), "CONNECTED") &&
        unhex(str(c, "controllerBootId"), previous, 16) &&
        integer(c, "connectionId", 1, UINT32_MAX, &id)) {
      if (memcmp(previous, boot, 16)) {
        if (publish_closed(c, "NRF_RESTART") == ESP_OK)
          recovering = false;
      } else if (recovering && !unresolved &&
                 gw_core_maintenance_acquire("gatt-recovery") == ESP_OK) {
        esp_err_t e =
            close_connection("recovery", boot, id, gw_core_mono_ms() + 1000);
        if ((e == ESP_OK || e == ESP_ERR_NOT_FOUND) &&
            publish_closed(c, "ESP_RESTART") == ESP_OK)
          recovering = false;
        gw_core_maintenance_release("gatt-recovery");
      } else if (!recovering) {
        esp_err_t e = info(boot, id, ci, deadline);
        if (e == ESP_ERR_NOT_FOUND)
          publish_closed(c, "DISCONNECTED");
      }
    } else if (recovering && !unresolved) {
      /* Covers the crash between successful CONNECT and registration. */
      uint8_t radio[15];
      size_t n = sizeof(radio);
      esp_err_t e = query(0x09, NULL, 0, radio, &n, deadline, NULL);
      if (e == ESP_OK && n == 15 && radio[14] == 0)
        recovering = false;
      else if (e == ESP_OK && n == 15 && radio[14] == 2 &&
               (id = gw_get_u32(radio + 10)) &&
               info(boot, id, ci, deadline) == ESP_OK) {
        cJSON *orphan = connection(boot, ci, "recovery");
        if (orphan) {
          persist(CONN_KEY, orphan);
          cJSON_Delete(orphan);
        }
      }
    }
    cJSON_Delete(c);
    xSemaphoreGive(lock);
  }
}
esp_err_t gw_gatt_nrf_init(void) {
  if (lock)
    return ESP_ERR_INVALID_STATE;
  lock = xSemaphoreCreateMutex();
  if (!lock)
    return ESP_ERR_NO_MEM;
  return xTaskCreate(monitor, "nrf_gatt_state", 8192, NULL, 4, NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}
