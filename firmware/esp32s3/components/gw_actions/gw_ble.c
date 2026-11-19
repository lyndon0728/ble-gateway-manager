#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_actions_internal.h"
#include "gw_authority.h"
#include "gw_ble_bonds.h"
#include "gw_ble_peripheral.h"
#include "gw_capture.h"
#include "gw_nrf.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CONFIG_BT_NIMBLE_SECURITY_ENABLE) ||                              \
    !CONFIG_BT_NIMBLE_SECURITY_ENABLE
#error "Existing-bond encryption requires NimBLE security support"
#endif
void ble_store_config_init(void);

typedef struct {
  uint8_t kind;
  uint16_t size;
  uint8_t data[536];
} report_t;
typedef struct {
  bool enabled;
  uint64_t revision;
  uint8_t mode;
  uint16_t interval;
  uint8_t data[31], response[31];
  size_t length, response_length;
} adv_t;
static QueueHandle_t reports;
static SemaphoreHandle_t filter_lock, done, radio_lock;
static struct gw_filter_v2_config filter_config;
static struct gw_filter_v2_state filter_state;
/* Callback ownership is bounded copying only. Timestamp allocation and queue
 * publication share this lock with the worker's watermark advancement. */
static struct {
  uint8_t length;
  uint8_t data[55];
} scan_input[32];
static unsigned scan_head, scan_count;
_Static_assert(sizeof(scan_input) + sizeof(filter_state) <= 4096,
               "PAIR buffers exceed the specified memory budget");
#define profile filter_config.legacy
static void clear_filter_locked(void) {
  filter_state.counters[GW_FILTER_CONFIG_DISCARDED] += scan_count;
  scan_head = scan_count = 0;
  gw_filter_v2_clear(&filter_state);
}
static adv_t advertising;
static bool ready, paused, fault;
static uint8_t own_type, boot[16], peer[6], peer_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t conn_id, next_conn_id, operation;
static char connection_ref[33];
static volatile bool waiting;
static bool disconnect_wait, encryption_wait;
static uint16_t peripheral_conn = BLE_HS_CONN_HANDLE_NONE, peripheral_tx_handle;
static uint32_t peripheral_id, next_peripheral_id, peripheral_close_pending;
static bool peripheral_subscribed, peripheral_restart;
static gw_ble_peripheral_receiver peripheral_receiver;
static void *peripheral_receiver_context;
static SemaphoreHandle_t peripheral_send_lock;
static uint64_t peripheral_last_seq;
static uint16_t peripheral_last_len;
static uint8_t peripheral_last_data[512];
static ble_store_read_fn *bond_store_read;
static ble_store_write_fn *bond_store_write;
static ble_store_delete_fn *bond_store_delete;
static int operation_status;
static uint32_t dropped_reports, dropped_notifications, filtered_reports;
static bool close_pending;
static report_t close_record;
static struct ble_npl_event advertise_event;
static uint16_t service_start, service_end, value_handle, char_end, ccc_handle;
static uint8_t properties;
static unsigned matches;
static ble_uuid128_t target_uuid;
static uint8_t read_value[512];
static size_t read_length;
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;

static int gap(struct ble_gap_event *event, void *arg);
static esp_err_t rc(int value) {
  return !value                     ? ESP_OK
         : value == BLE_HS_ETIMEOUT ? ESP_ERR_TIMEOUT
         : value == BLE_HS_EBUSY    ? ESP_ERR_INVALID_STATE
                                    : ESP_FAIL;
}
static void complete(uint32_t token, int status) {
  portENTER_CRITICAL(&state_mux);
  bool signal = waiting && operation == token;
  if (signal) {
    waiting = false;
    operation_status = status;
  }
  portEXIT_CRITICAL(&state_mux);
  if (signal)
    xSemaphoreGive(done);
}
static void *begin_operation(uint64_t deadline) {
  if (gw_core_mono_ms() >= deadline)
    return NULL;
  xSemaphoreTake(done, 0);
  portENTER_CRITICAL(&state_mux);
  if (operation == UINT32_MAX) {
    fault = true;
    portEXIT_CRITICAL(&state_mux);
    return NULL;
  }
  operation++;
  waiting = true;
  operation_status = BLE_HS_ETIMEOUT;
  portEXIT_CRITICAL(&state_mux);
  return (void *)(uintptr_t)operation;
}
static esp_err_t wait_operation(int start, uint64_t deadline) {
  if (start) {
    complete(operation, start);
    return rc(start);
  }
  uint64_t now = gw_core_mono_ms();
  if (now >= deadline ||
      xSemaphoreTake(done, pdMS_TO_TICKS(deadline - now)) != pdTRUE) {
    portENTER_CRITICAL(&state_mux);
    waiting = false;
    portEXIT_CRITICAL(&state_mux);
    return ESP_ERR_TIMEOUT;
  }
  return rc(operation_status);
}
static void host(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}
static void synchronized(void) {
  if (ble_hs_util_ensure_addr(0) == 0 &&
      ble_hs_id_infer_auto(0, &own_type) == 0)
    ready = true;
  else
    fault = true;
}
static void reset(int reason) {
  (void)reason;
  ready = false;
  fault = true;
}
static void adv_start_event(struct ble_npl_event *event) {
  uint32_t token = (uint32_t)(uintptr_t)ble_npl_event_get_arg(event);
  if (!waiting || operation != token)
    return;
  struct ble_gap_adv_params p = {
      .conn_mode =
          advertising.mode == 2 ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON,
      .disc_mode =
          advertising.mode ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON,
      .itvl_min = advertising.interval,
      .itvl_max = advertising.interval,
      .channel_map = 7};
  complete(token,
           ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &p, gap, NULL));
}
static esp_err_t apply_advertising(uint64_t deadline) {
  if (fault || gw_core_mono_ms() >= deadline)
    return ESP_ERR_INVALID_STATE;
  if (advertising.enabled && !paused && ble_gap_disc_active())
    return ESP_ERR_INVALID_STATE;
  int result = ble_gap_adv_stop();
  if (result && result != BLE_HS_EALREADY)
    return rc(result);
  if (!advertising.enabled || paused ||
      peripheral_conn != BLE_HS_CONN_HANDLE_NONE)
    return ble_gap_adv_active() ? ESP_ERR_INVALID_STATE : ESP_OK;
  if ((result = ble_gap_adv_set_data(advertising.data,
                                     (int)advertising.length)) != 0)
    return rc(result);
  if ((result = ble_gap_adv_rsp_set_data(
           advertising.response, (int)advertising.response_length)) != 0)
    return rc(result);
  void *token = begin_operation(deadline);
  if (!token)
    return ESP_ERR_INVALID_STATE;
  ble_npl_event_init(&advertise_event, adv_start_event, token);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &advertise_event);
  esp_err_t e = wait_operation(0, deadline);
  if (e == ESP_ERR_TIMEOUT)
    fault = true;
  return e == ESP_OK && !ble_gap_adv_active() ? ESP_FAIL : e;
}
static esp_err_t apply_scan(void) {
  if ((profile.scan_flags & 1) && !paused && ble_gap_adv_active())
    return ESP_ERR_INVALID_STATE;
  int result = ble_gap_disc_cancel();
  if (result && result != BLE_HS_EALREADY)
    return rc(result);
  bool enabled = (profile.scan_flags & 1) && !paused &&
                 conn == BLE_HS_CONN_HANDLE_NONE &&
                 peripheral_conn == BLE_HS_CONN_HANDLE_NONE;
  if (!enabled) {
    xSemaphoreTake(filter_lock, portMAX_DELAY);
    clear_filter_locked();
    xSemaphoreGive(filter_lock);
    gw_capture_radio_enabled(1, false);
    return ESP_OK;
  }
  struct ble_gap_disc_params p = {.itvl = profile.interval_ticks,
                                  .window = profile.window_ticks,
                                  .passive = !(profile.scan_flags & 2),
                                  .filter_duplicates = 0};
  result = ble_gap_disc(own_type, BLE_HS_FOREVER, &p, gap, NULL);
  gw_capture_radio_enabled(1, result == 0);
  return rc(result);
}
static void enqueue_scan(const struct ble_gap_disc_desc *d) {
  if (d->length_data > 31) {
    dropped_reports++;
    return;
  }
  uint8_t kind = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP ? 1 : 0;
  int8_t rssi = d->rssi == 127 ? -128 : d->rssi;
  if (xSemaphoreTake(filter_lock, 0) != pdTRUE) {
    dropped_reports++;
    return;
  }
  if (scan_count == 32) {
    filter_state.counters[GW_FILTER_SEEN]++;
    filter_state.counters[GW_FILTER_PAIR_OVERFLOW]++;
    dropped_reports++;
    xSemaphoreGive(filter_lock);
    return;
  }
  report_t r = {.kind = 0, .size = (uint16_t)(24 + d->length_data)};
  gw_put_u64(r.data, gw_core_mono_ms());
  r.data[8] = kind;
  r.data[9] = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND    ? 3
               : d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND  ? 5
               : d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND ? 2
                                                                  : 0);
  /* Identity-resolved reports are not presented as an over-the-air address. */
  r.data[10] = d->addr.type <= 1 ? d->addr.type : 255;
  if (r.data[10] != 255)
    for (unsigned i = 0; i < 6; i++)
      r.data[11 + i] = d->addr.val[5 - i];
  r.data[17] = (uint8_t)rssi;
  r.data[18] = 128;
  r.data[19] = 1;
  r.data[20] = 0;
  r.data[21] = 255;
  gw_put_u16(r.data + 22, d->length_data);
  memcpy(r.data + 24, d->data, d->length_data);
  unsigned tail = (scan_head + scan_count) % 32;
  scan_input[tail].length = (uint8_t)r.size;
  memcpy(scan_input[tail].data, r.data, r.size);
  scan_count++;
  filter_state.counters[GW_FILTER_SEEN]++;
  xSemaphoreGive(filter_lock);
}
/* The stack must never start pairing on a remote Security Request. Only the
 * explicit application path below may read PEER_SEC and restart encryption.
 * OUR_SEC reads remain available for a peripheral's pre-provisioned LTK. */
static int existing_bond_read(int kind, const union ble_store_key *key,
                              union ble_store_value *value) {
  if (kind == BLE_STORE_OBJ_TYPE_PEER_SEC && conn != BLE_HS_CONN_HANDLE_NONE)
    return BLE_HS_EAUTHEN;
  return bond_store_read ? bond_store_read(kind, key, value) : BLE_HS_ENOENT;
}
static int existing_bond_write(int kind, const union ble_store_value *value) {
  if (kind == BLE_STORE_OBJ_TYPE_PEER_SEC || kind == BLE_STORE_OBJ_TYPE_OUR_SEC)
    return BLE_HS_EAUTHEN;
  return bond_store_write ? bond_store_write(kind, value) : BLE_HS_ENOTSUP;
}
static int existing_bond_delete(int kind, const union ble_store_key *key) {
  if (kind == BLE_STORE_OBJ_TYPE_PEER_SEC || kind == BLE_STORE_OBJ_TYPE_OUR_SEC)
    return BLE_HS_EAUTHEN;
  return bond_store_delete ? bond_store_delete(kind, key) : BLE_HS_ENOTSUP;
}
static int peripheral_access(uint16_t handle, uint16_t attribute,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)attribute;
  if ((uintptr_t)arg == 1 && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    uint8_t info[13] = {1, 7};
    gw_put_u64(info + 5, gw_core_mono_ms());
    return os_mbuf_append(ctxt->om, info, sizeof(info))
               ? BLE_ATT_ERR_INSUFFICIENT_RES
               : 0;
  }
  if ((uintptr_t)arg != 2 || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
      ctxt->offset)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  size_t length = OS_MBUF_PKTLEN(ctxt->om);
  uint16_t mtu = ble_att_mtu(handle);
  if (!length || length > 512 || mtu < 3 || length > (size_t)mtu - 3)
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  if (paused || handle != peripheral_conn || !peripheral_id ||
      !peripheral_receiver)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  uint8_t data[512];
  if (os_mbuf_copydata(ctxt->om, 0, length, data))
    return BLE_ATT_ERR_UNLIKELY;
  return peripheral_receiver(peripheral_id, data, length, mtu,
                             peripheral_receiver_context) == ESP_OK
             ? 0
             : BLE_ATT_ERR_INSUFFICIENT_RES;
}
#define PERIPHERAL_UUID(last)                                                  \
  BLE_UUID128_INIT(0x00, 0x54, 0x0b, 0x9a, 0x2f, 0x6c, 0x91, 0x9d, 0x31, 0x4c, \
                   0x5a, 0x4b, last, 0x00, 0xe0, 0x7b)
static const ble_uuid128_t peripheral_service_uuid = PERIPHERAL_UUID(1),
                           peripheral_info_uuid = PERIPHERAL_UUID(2),
                           peripheral_rx_uuid = PERIPHERAL_UUID(3),
                           peripheral_tx_uuid = PERIPHERAL_UUID(4);
static const struct ble_gatt_svc_def peripheral_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &peripheral_service_uuid.u,
     .characteristics =
         (struct ble_gatt_chr_def[]){{.uuid = &peripheral_info_uuid.u,
                                      .access_cb = peripheral_access,
                                      .arg = (void *)1,
                                      .flags = BLE_GATT_CHR_F_READ},
                                     {.uuid = &peripheral_rx_uuid.u,
                                      .access_cb = peripheral_access,
                                      .arg = (void *)2,
                                      .flags = BLE_GATT_CHR_F_WRITE},
                                     {.uuid = &peripheral_tx_uuid.u,
                                      .access_cb = peripheral_access,
                                      .flags = BLE_GATT_CHR_F_NOTIFY,
                                      .val_handle = &peripheral_tx_handle},
                                     {0}}},
    {0}};
void gw_ble_peripheral_set_receiver(gw_ble_peripheral_receiver receiver,
                                    void *context) {
  portENTER_CRITICAL(&state_mux);
  peripheral_receiver = receiver;
  peripheral_receiver_context = context;
  portEXIT_CRITICAL(&state_mux);
}
bool gw_ble_peripheral_identity(uint32_t id, uint8_t out_boot[16],
                                uint8_t address[6], uint8_t *address_type,
                                uint8_t *level, uint16_t *mtu) {
  portENTER_CRITICAL(&state_mux);
  uint16_t handle = peripheral_conn;
  bool valid = id && id == peripheral_id && handle != BLE_HS_CONN_HANDLE_NONE;
  portEXIT_CRITICAL(&state_mux);
  struct ble_gap_conn_desc desc;
  if (!valid || ble_gap_conn_find(handle, &desc))
    return false;
  if (out_boot)
    memcpy(out_boot, boot, 16);
  if (address)
    for (unsigned i = 0; i < 6; i++)
      address[i] = desc.peer_ota_addr.val[5 - i];
  if (address_type)
    *address_type = desc.peer_ota_addr.type;
  if (level)
    *level =
        desc.sec_state.encrypted ? (desc.sec_state.authenticated ? 3 : 2) : 1;
  if (mtu)
    *mtu = ble_att_mtu(handle);
  return peripheral_id == id && peripheral_conn == handle;
}
esp_err_t gw_ble_peripheral_send(uint32_t id, uint64_t seq, const uint8_t *data,
                                 size_t length) {
  if (!seq || !data || !length || length > 512)
    return ESP_ERR_INVALID_ARG;
  if (!peripheral_send_lock ||
      xSemaphoreTake(peripheral_send_lock, 0) != pdTRUE)
    return ESP_ERR_INVALID_STATE;
  esp_err_t result = ESP_ERR_INVALID_STATE;
  uint16_t handle = peripheral_conn, mtu;
  if (!gw_ble_peripheral_identity(id, NULL, NULL, NULL, NULL, &mtu))
    goto end;
  if (length > (size_t)mtu - 3) {
    result = ESP_ERR_INVALID_SIZE;
    goto end;
  }
  if (seq == peripheral_last_seq) {
    result = length == peripheral_last_len &&
                     !memcmp(data, peripheral_last_data, length)
                 ? ESP_OK
                 : ESP_ERR_INVALID_ARG;
    goto end;
  }
  if (seq < peripheral_last_seq || !peripheral_subscribed || paused)
    goto end;
  struct os_mbuf *value = ble_hs_mbuf_from_flat(data, length);
  if (!value) {
    result = ESP_ERR_NO_MEM;
    goto end;
  }
  result = rc(ble_gatts_notify_custom(handle, peripheral_tx_handle, value));
  if (result == ESP_OK && id == peripheral_id && handle == peripheral_conn) {
    peripheral_last_seq = seq;
    peripheral_last_len = length;
    memcpy(peripheral_last_data, data, length);
  } else if (result == ESP_OK)
    result = ESP_ERR_TIMEOUT;
end:
  xSemaphoreGive(peripheral_send_lock);
  return result;
}
esp_err_t gw_ble_peripheral_close(uint32_t id) {
  if (!id || id != peripheral_id || peripheral_conn == BLE_HS_CONN_HANDLE_NONE)
    return ESP_ERR_NOT_FOUND;
  return rc(ble_gap_terminate(peripheral_conn, BLE_ERR_REM_USER_CONN_TERM));
}
static int gap(struct ble_gap_event *e, void *arg) {
  uint32_t token = (uint32_t)(uintptr_t)arg;
  switch (e->type) {
  case BLE_GAP_EVENT_DISC:
    enqueue_scan(&e->disc);
    break;
  case BLE_GAP_EVENT_CONNECT:
    if (e->connect.status) {
      complete(token, e->connect.status);
      break;
    }
    if (!token) {
      struct ble_gap_conn_desc desc;
      if (paused || !advertising.enabled || advertising.mode != 2 ||
          conn != BLE_HS_CONN_HANDLE_NONE ||
          peripheral_conn != BLE_HS_CONN_HANDLE_NONE ||
          peripheral_close_pending || next_peripheral_id == UINT32_MAX ||
          ble_gap_conn_find(e->connect.conn_handle, &desc) ||
          desc.role != BLE_GAP_ROLE_SLAVE) {
        ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
      }
      (void)ble_gap_disc_cancel();
      gw_capture_radio_enabled(1, false);
      xSemaphoreTake(filter_lock, portMAX_DELAY);
      clear_filter_locked();
      xSemaphoreGive(filter_lock);
      portENTER_CRITICAL(&state_mux);
      peripheral_conn = e->connect.conn_handle;
      peripheral_id = ++next_peripheral_id;
      peripheral_subscribed = false;
      peripheral_last_seq = 0;
      portEXIT_CRITICAL(&state_mux);
      break;
    }
    if (!waiting || token != operation) {
      ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      break;
    }
    conn = e->connect.conn_handle;
    complete(token, 0);
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    if (e->disconnect.conn.conn_handle == peripheral_conn) {
      portENTER_CRITICAL(&state_mux);
      peripheral_close_pending = peripheral_id;
      peripheral_id = 0;
      peripheral_conn = BLE_HS_CONN_HANDLE_NONE;
      peripheral_subscribed = false;
      peripheral_restart = true;
      portEXIT_CRITICAL(&state_mux);
      break;
    }
    if (e->disconnect.conn.conn_handle == conn) {
      if (conn_id) {
        report_t r = {.kind = 2, .size = 13};
        gw_put_u32(r.data, conn_id);
        r.data[4] = peer_type;
        memcpy(r.data + 5, peer, 6);
        gw_put_u16(r.data + 11, (uint16_t)e->disconnect.reason);
        portENTER_CRITICAL(&state_mux);
        close_record = r;
        close_pending = true;
        portEXIT_CRITICAL(&state_mux);
      }
      conn = BLE_HS_CONN_HANDLE_NONE;
      conn_id = 0;
      connection_ref[0] = 0;
      complete(operation, disconnect_wait ? 0 : BLE_HS_ENOTCONN);
    }
    break;
  case BLE_GAP_EVENT_NOTIFY_RX:
    if (e->notify_rx.conn_handle == conn && conn_id) {
      size_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
      if (n > 512) {
        dropped_notifications++;
        break;
      }
      report_t r = {.kind = 1, .size = (uint16_t)(24 + n)};
      gw_put_u64(r.data, gw_core_mono_ms());
      gw_put_u32(r.data + 8, conn_id);
      r.data[12] = peer_type;
      memcpy(r.data + 13, peer, 6);
      gw_put_u16(r.data + 19, e->notify_rx.attr_handle);
      r.data[21] = e->notify_rx.indication ? 1 : 0;
      gw_put_u16(r.data + 22, (uint16_t)n);
      if (os_mbuf_copydata(e->notify_rx.om, 0, (int)n, r.data + 24) ||
          xQueueSend(reports, &r, 0) != pdTRUE)
        dropped_notifications++;
    }
    break;
  case BLE_GAP_EVENT_ENC_CHANGE:
    if (e->enc_change.conn_handle == conn && encryption_wait)
      complete(operation, e->enc_change.status);
    break;
  case BLE_GAP_EVENT_SUBSCRIBE:
    if (e->subscribe.conn_handle == peripheral_conn &&
        e->subscribe.attr_handle == peripheral_tx_handle)
      peripheral_subscribed = e->subscribe.cur_notify;
    break;
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  case BLE_GAP_EVENT_PASSKEY_ACTION:
    ble_gap_terminate(e->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
    break;
  default:
    break;
  }
  return 0;
}
typedef struct {
  unsigned count;
  uint8_t length[2], records[2][55];
} scan_emissions_t;
static void collect_scan(void *context, const uint8_t *record, size_t length) {
  scan_emissions_t *out = context;
  if (out->count < 2 && length <= 55) {
    memcpy(out->records[out->count], record, length);
    out->length[out->count++] = (uint8_t)length;
  }
}
static void drain_scan(void) {
  for (unsigned budget = 0; budget < 32; budget++) {
    scan_emissions_t out = {0};
    xSemaphoreTake(filter_lock, portMAX_DELAY);
    if (!scan_count) {
      /* No callback can publish an earlier timestamp while this lock is held.
       */
      gw_filter_v2_advance(&filter_state, gw_core_mono_ms());
      xSemaphoreGive(filter_lock);
      return;
    }
    unsigned head = scan_head;
    scan_head = (scan_head + 1) % 32;
    scan_count--;
    filter_state.counters[GW_FILTER_SEEN]--;
    gw_filter_v2_feed(&filter_state, &filter_config, scan_input[head].data,
                      scan_input[head].length, collect_scan, &out);
    xSemaphoreGive(filter_lock);
    for (unsigned i = 0; i < out.count; i++)
      if (gw_capture_esp(out.records[i], out.length[i]) != ESP_OK)
        dropped_reports++;
  }
}
static void report_worker(void *arg) {
  (void)arg;
  report_t r;
  for (;;) {
    drain_scan();
    if (peripheral_close_pending && peripheral_receiver &&
        peripheral_receiver(peripheral_close_pending, NULL, 0, 0,
                            peripheral_receiver_context) == ESP_OK)
      peripheral_close_pending = 0;
    /* Serialize restoration with complete control actions, without borrowing
     * their asynchronous completion semaphore. */
    if (peripheral_restart && !peripheral_close_pending && !waiting &&
        !paused && xSemaphoreTake(radio_lock, 0) == pdTRUE) {
      peripheral_restart = false;
      (void)apply_scan();
      if (advertising.enabled && !ble_gap_adv_active()) {
        struct ble_gap_adv_params p = {
            .conn_mode = advertising.mode == 2 ? BLE_GAP_CONN_MODE_UND
                                               : BLE_GAP_CONN_MODE_NON,
            .disc_mode = advertising.mode ? BLE_GAP_DISC_MODE_GEN
                                          : BLE_GAP_DISC_MODE_NON,
            .itvl_min = advertising.interval,
            .itvl_max = advertising.interval,
            .channel_map = 7};
        if (ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &p, gap, NULL))
          fault = true;
      }
      xSemaphoreGive(radio_lock);
    }
    if (xQueueReceive(reports, &r, pdMS_TO_TICKS(50)) == pdTRUE) {
      esp_err_t e = r.kind ? gw_capture_esp_gatt(boot, r.data, r.size)
                           : gw_capture_esp(r.data, r.size);
      if (e != ESP_OK) {
        if (r.kind)
          dropped_notifications++;
        else
          dropped_reports++;
      }
    }
    portENTER_CRITICAL(&state_mux);
    bool pending = close_pending;
    if (pending)
      r = close_record;
    portEXIT_CRITICAL(&state_mux);
    if (pending && gw_capture_link_down(1, boot, gw_get_u32(r.data), r.data[4],
                                        r.data + 5, "DISCONNECTED") == ESP_OK) {
      if (gw_store_delete("gatt/esp-connection") == ESP_OK) {
        portENTER_CRITICAL(&state_mux);
        close_pending = false;
        portEXIT_CRITICAL(&state_mux);
      }
    }
  }
}
static esp_err_t await_ready(uint64_t deadline) {
  while (!ready && !fault && gw_core_mono_ms() < deadline)
    vTaskDelay(pdMS_TO_TICKS(10));
  return ready && !fault && gw_core_mono_ms() < deadline
             ? ESP_OK
             : ESP_ERR_INVALID_STATE;
}
esp_err_t gw_ble_init(void) {
  esp_err_t e = gw_ble_bonds_check(false);
  if (e != ESP_OK)
    return e;
  gw_capture_esp_boot(boot);
  gw_filter_v2_default(&filter_config);
  filter_lock = xSemaphoreCreateMutex();
  done = xSemaphoreCreateBinary();
  peripheral_send_lock = xSemaphoreCreateMutex();
  radio_lock = xSemaphoreCreateMutex();
  reports = xQueueCreate(32, sizeof(report_t));
  if (!filter_lock || !done || !reports || !peripheral_send_lock || !radio_lock)
    return ESP_ERR_NO_MEM;
  cJSON *old = gw_store_get_json("gatt/esp-connection");
  if (old) {
    uint8_t old_boot[16], address[6];
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(old, "id");
    if (!gw_action_hex(gw_action_string(old, "boot"), old_boot, 16) ||
        !gw_action_hex(gw_action_string(old, "address"), address, 6) ||
        !cJSON_IsNumber(id) ||
        gw_capture_link_down(
            1, old_boot, (uint32_t)id->valuedouble,
            !strcmp(gw_action_string(old, "addressType"), "RANDOM"), address,
            "ESP_RESTART") != ESP_OK) {
      cJSON_Delete(old);
      return ESP_FAIL;
    }
    cJSON_Delete(old);
    if (gw_store_delete("gatt/esp-connection") != ESP_OK)
      return ESP_FAIL;
  }
  e = nimble_port_init();
  if (e != ESP_OK)
    return e;
  ble_hs_cfg.sync_cb = synchronized;
  ble_hs_cfg.reset_cb = reset;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  /* IDF 5.5 NimBLE rejects incoming Pair Requests at security level 1.
   * Encryption restore using an existing LTK is still explicitly supported. */
  ble_hs_cfg.sm_sec_lvl = 1;
  ble_store_config_init();
  e = gw_ble_bonds_check(true);
  if (e != ESP_OK) {
    fault = true;
    return e;
  }
  bond_store_read = ble_hs_cfg.store_read_cb;
  bond_store_write = ble_hs_cfg.store_write_cb;
  bond_store_delete = ble_hs_cfg.store_delete_cb;
  ble_hs_cfg.store_read_cb = existing_bond_read;
  ble_hs_cfg.store_write_cb = existing_bond_write;
  ble_hs_cfg.store_delete_cb = existing_bond_delete;
  if (ble_gatts_count_cfg(peripheral_services) ||
      ble_gatts_add_svcs(peripheral_services))
    return ESP_FAIL;
  if (xTaskCreate(report_worker, "esp_ble_capture", 6144, NULL, 4, NULL) !=
      pdPASS)
    return ESP_ERR_NO_MEM;
  nimble_port_freertos_init(host);
  return ESP_OK;
}
static esp_err_t ble_configure_locked(const cJSON *o, bool adv,
                                      uint64_t revision, uint64_t deadline,
                                      cJSON **details) {
  esp_err_t e = await_ready(deadline);
  if (e != ESP_OK)
    return e;
  if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending || paused)
    return ESP_ERR_INVALID_STATE;
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(o, "scan"), "enabled")) &&
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(o, "advertising"), "enabled")))
    return ESP_ERR_INVALID_STATE;
  if (adv) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "advertising");
    const char *mode = gw_action_string(a, "mode");
    adv_t next = {.revision = revision};
    if (!strcmp(mode, "SCANNABLE_NONCONNECTABLE"))
      next.mode = 1;
    else if (!strcmp(mode, "CONNECTABLE_SCANNABLE"))
      next.mode = 2;
    else if (strcmp(mode, "NONCONNECTABLE"))
      return ESP_ERR_NOT_SUPPORTED;
    if (next.mode == 2 && !gw_authority_has("ESP32_BLE_PERIPHERAL"))
      return ESP_ERR_NOT_SUPPORTED;
    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(a, "intervalMs"),
                *enabled = cJSON_GetObjectItemCaseSensitive(a, "enabled");
    if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(interval) ||
        interval->valuedouble < 100 || interval->valuedouble > 10240 ||
        (int)interval->valuedouble != interval->valuedouble)
      return ESP_ERR_INVALID_ARG;
    next.enabled = cJSON_IsTrue(enabled);
    next.interval = (uint16_t)(((int)interval->valuedouble * 8 + 2) / 5);
    const char *data = gw_action_string(a, "advDataBase64"),
               *response = gw_action_string(a, "scanResponseBase64");
    if (mbedtls_base64_decode(next.data, 31, &next.length,
                              (const unsigned char *)data, strlen(data)) ||
        mbedtls_base64_decode(next.response, 31, &next.response_length,
                              (const unsigned char *)response,
                              strlen(response)) ||
        !gw_ad_valid(next.data, next.length) ||
        !gw_ad_valid(next.response, next.response_length) ||
        (!next.mode && next.response_length))
      return ESP_ERR_INVALID_ARG;
    if (revision < advertising.revision)
      return ESP_ERR_INVALID_STATE;
    if (revision == advertising.revision) {
      if (next.enabled != advertising.enabled ||
          next.mode != advertising.mode ||
          next.interval != advertising.interval ||
          next.length != advertising.length ||
          next.response_length != advertising.response_length ||
          memcmp(next.data, advertising.data, next.length) ||
          memcmp(next.response, advertising.response, next.response_length))
        return ESP_ERR_INVALID_STATE;
      if (next.enabled && !ble_gap_adv_active() &&
          peripheral_conn == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;
      *details = gw_ble_diagnostics();
      return *details ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (next.enabled && peripheral_conn != BLE_HS_CONN_HANDLE_NONE)
      return ESP_ERR_INVALID_STATE;
    adv_t previous = advertising;
    advertising = next;
    e = apply_advertising(deadline);
    if (e != ESP_OK) {
      advertising = previous;
      if (apply_advertising(gw_core_mono_ms() + 1500) != ESP_OK)
        fault = true;
      return e;
    }
  } else {
    struct gw_filter_v2_config *next = malloc(sizeof(*next)),
                               *previous = malloc(sizeof(*previous));
    uint8_t *encoded = malloc(GW_FILTER_V2_MAX);
    size_t length = 0;
    if (!next || !previous || !encoded) {
      free(next);
      free(previous);
      free(encoded);
      return ESP_ERR_NO_MEM;
    }
    e = gw_nrf_compile_scan(o, revision, encoded, GW_FILTER_V2_MAX, &length);
    if (e == ESP_OK && encoded[1] && !gw_authority_has("ESP32_BLE_FILTER_V2"))
      e = ESP_ERR_NOT_SUPPORTED;
    if (e == ESP_OK && !gw_filter_v2_parse(encoded, length, next))
      e = ESP_ERR_INVALID_ARG;
    if (e != ESP_OK) {
      free(next);
      free(previous);
      free(encoded);
      return e;
    }
    xSemaphoreTake(filter_lock, portMAX_DELAY);
    *previous = filter_config;
    xSemaphoreGive(filter_lock);
    if (revision <= previous->legacy.revision) {
      uint8_t *old_encoded = malloc(GW_FILTER_V2_MAX);
      size_t old_length =
          old_encoded
              ? gw_filter_v2_encode(previous, old_encoded, GW_FILTER_V2_MAX)
              : 0;
      bool same = revision == previous->legacy.revision &&
                  old_length == length && old_length &&
                  !memcmp(old_encoded, encoded, length);
      free(old_encoded);
      free(encoded);
      free(next);
      free(previous);
      if (!same)
        return ESP_ERR_INVALID_STATE;
      *details = gw_ble_diagnostics();
      return *details ? ESP_OK : ESP_ERR_NO_MEM;
    }
    free(encoded);
    xSemaphoreTake(filter_lock, portMAX_DELAY);
    clear_filter_locked();
    filter_config = *next;
    xSemaphoreGive(filter_lock);
    free(next);
    e = apply_scan();
    if (e != ESP_OK) {
      xSemaphoreTake(filter_lock, portMAX_DELAY);
      clear_filter_locked();
      filter_config = *previous;
      xSemaphoreGive(filter_lock);
      if (apply_scan() != ESP_OK)
        fault = true;
    }
    free(previous);
    if (e != ESP_OK)
      return e;
  }
  *details = gw_ble_diagnostics();
  return *details ? ESP_OK : ESP_ERR_NO_MEM;
}
cJSON *gw_ble_diagnostics(void) {
  cJSON *j = cJSON_CreateObject();
  if (!j)
    return NULL;
  cJSON_AddStringToObject(j, "radio", "ESP32");
  cJSON_AddBoolToObject(j, "hostReady", ready);
  cJSON_AddBoolToObject(j, "fault", fault);
  cJSON_AddBoolToObject(j, "scanning", ready && ble_gap_disc_active());
  cJSON_AddBoolToObject(j, "advertising", ready && ble_gap_adv_active());
  cJSON_AddBoolToObject(j, "maintenancePaused", paused);
  cJSON_AddBoolToObject(j, "peripheralConnected",
                        peripheral_conn != BLE_HS_CONN_HANDLE_NONE);
  cJSON_AddBoolToObject(j, "peripheralSubscribed", peripheral_subscribed);
  char adv_revision[24];
  snprintf(adv_revision, sizeof(adv_revision), "%llu",
           (unsigned long long)advertising.revision);
  cJSON_AddStringToObject(j, "advertisingRevision", adv_revision);
  cJSON_AddStringToObject(j, "advertisingState",
                          !advertising.enabled ? "STOPPED"
                          : peripheral_conn != BLE_HS_CONN_HANDLE_NONE || paused
                              ? "PAUSED"
                          : ble_gap_adv_active() ? "ACTIVE"
                                                 : "FAULT");
  if (advertising.enabled &&
      (peripheral_conn != BLE_HS_CONN_HANDLE_NONE || paused))
    cJSON_AddStringToObject(j, "advertisingPauseReason",
                            paused ? "MAINTENANCE" : "PERIPHERAL_CONNECTION");
  cJSON_AddNumberToObject(j, "droppedReports", dropped_reports);
  cJSON_AddNumberToObject(j, "droppedNotifications", dropped_notifications);
  xSemaphoreTake(filter_lock, portMAX_DELAY);
  uint8_t status[96];
  gw_filter_v2_status(&filter_state, &filter_config, status);
  xSemaphoreGive(filter_lock);
  filtered_reports = (uint32_t)gw_get_u64(status + 32);
  cJSON_AddNumberToObject(j, "filteredReports", filtered_reports);
  cJSON *filter = cJSON_AddObjectToObject(j, "scanFilterStatus");
  char counter[24];
  snprintf(counter, sizeof(counter), "%llu",
           (unsigned long long)gw_get_u64(status));
  cJSON_AddStringToObject(filter, "revision", counter);
  cJSON_AddStringToObject(filter, "engine", status[8] ? "V2" : "LEGACY");
  cJSON_AddStringToObject(filter, "scope",
                          status[9] ? "ADV_SCAN_RSP_PAIR" : "REPORT");
  cJSON_AddNumberToObject(filter, "pendingCount", gw_get_u16(status + 10));
  cJSON_AddNumberToObject(filter, "pendingBytes", gw_get_u32(status + 12));
  static const char *names[] = {
      "recordsSeen",        "recordsPassed",  "ruleDropped", "pairTimeout",
      "pairAmbiguous",      "pairOverflow",   "malformedAd", "unknownRssi",
      "unsupportedContext", "configDiscarded"};
  for (unsigned i = 0; i < 10; i++) {
    snprintf(counter, sizeof(counter), "%llu",
             (unsigned long long)gw_get_u64(status + 16 + 8 * i));
    cJSON_AddStringToObject(filter, names[i], counter);
  }
  cJSON_AddStringToObject(j, "connectionRef", connection_ref);
  return j;
}
static esp_err_t ble_pause_locked(uint64_t deadline) {
  if (!ready)
    return ESP_OK;
  if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending ||
      peripheral_conn != BLE_HS_CONN_HANDLE_NONE || peripheral_close_pending)
    return ESP_ERR_INVALID_STATE;
  paused = true;
  esp_err_t e = apply_scan();
  if (e == ESP_OK)
    e = apply_advertising(deadline);
  return e;
}
static esp_err_t ble_restore_locked(uint64_t deadline) {
  if (!ready)
    return ESP_OK;
  paused = false;
  esp_err_t e = apply_scan();
  if (e == ESP_OK)
    e = apply_advertising(deadline);
  return e;
}

static bool uuid128(const char *text, ble_uuid128_t *uuid) {
  uint8_t big[16];
  if (!gw_action_uuid(text, big))
    return false;
  uuid->u.type = BLE_UUID_TYPE_128;
  for (unsigned i = 0; i < 16; i++)
    uuid->value[i] = big[15 - i];
  return true;
}
static int svc_callback(uint16_t handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *svc, void *arg) {
  (void)handle;
  uint32_t token = (uint32_t)(uintptr_t)arg;
  if (!waiting || operation != token)
    return 0;
  if (!error->status && svc) {
    matches++;
    service_start = svc->start_handle;
    service_end = svc->end_handle;
    return 0;
  }
  complete(token, error->status == BLE_HS_EDONE
                      ? (matches == 1 ? 0 : BLE_HS_ENOENT)
                      : error->status);
  return 0;
}
static int chr_callback(uint16_t handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg) {
  (void)handle;
  uint32_t token = (uint32_t)(uintptr_t)arg;
  if (!waiting || operation != token)
    return 0;
  if (!error->status && chr) {
    if (value_handle && chr->def_handle > value_handle &&
        chr->def_handle <= char_end)
      char_end = chr->def_handle - 1;
    if (!ble_uuid_cmp(&chr->uuid.u, &target_uuid.u)) {
      matches++;
      value_handle = chr->val_handle;
      properties = chr->properties;
    }
    return 0;
  }
  complete(token, error->status == BLE_HS_EDONE
                      ? (matches == 1 ? 0 : BLE_HS_ENOENT)
                      : error->status);
  return 0;
}
static int dsc_callback(uint16_t handle, const struct ble_gatt_error *error,
                        uint16_t char_handle, const struct ble_gatt_dsc *dsc,
                        void *arg) {
  (void)handle;
  (void)char_handle;
  uint32_t token = (uint32_t)(uintptr_t)arg;
  if (!waiting || operation != token)
    return 0;
  if (!error->status && dsc) {
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
      matches++;
      ccc_handle = dsc->handle;
    }
    return 0;
  }
  complete(token, error->status == BLE_HS_EDONE
                      ? (matches == 1 ? 0 : BLE_HS_ENOENT)
                      : error->status);
  return 0;
}
static int read_callback(uint16_t handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  (void)handle;
  uint32_t token = (uint32_t)(uintptr_t)arg;
  if (!waiting || operation != token)
    return 0;
  if (!error->status && attr) {
    size_t n = OS_MBUF_PKTLEN(attr->om);
    if (attr->offset != read_length || read_length + n > 512 ||
        os_mbuf_copydata(attr->om, 0, (int)n, read_value + read_length)) {
      complete(token, BLE_HS_EMSGSIZE);
      return BLE_HS_EMSGSIZE;
    }
    read_length += n;
    return 0;
  }
  complete(token, error->status == BLE_HS_EDONE ? 0 : error->status);
  return 0;
}
static int write_callback(uint16_t handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
  (void)handle;
  (void)attr;
  complete((uint32_t)(uintptr_t)arg, error->status);
  return 0;
}
static esp_err_t disconnect(uint64_t deadline) {
  if (conn == BLE_HS_CONN_HANDLE_NONE)
    return ESP_OK;
  void *token = begin_operation(deadline);
  if (!token)
    return ESP_ERR_INVALID_STATE;
  disconnect_wait = true;
  esp_err_t e = wait_operation(
      ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM), deadline);
  disconnect_wait = false;
  return e == ESP_OK && conn != BLE_HS_CONN_HANDLE_NONE ? ESP_ERR_TIMEOUT : e;
}
static void add_base64(cJSON *o, const char *key, const uint8_t *value,
                       size_t n) {
  unsigned char text[685];
  size_t length = 0;
  if (mbedtls_base64_encode(text, sizeof(text), &length, value, n) == 0) {
    text[length] = 0;
    cJSON_AddStringToObject(o, key, (const char *)text);
  }
}
static esp_err_t discover(const cJSON *params, uint64_t deadline, bool ccc) {
  ble_uuid128_t service;
  if (!uuid128(gw_action_string(params, "serviceUuid"), &service) ||
      !uuid128(gw_action_string(params, "characteristicUuid"), &target_uuid))
    return ESP_ERR_INVALID_ARG;
  matches = 0;
  void *token = begin_operation(deadline);
  if (!token)
    return ESP_ERR_INVALID_STATE;
  esp_err_t e = wait_operation(
      ble_gattc_disc_svc_by_uuid(conn, &service.u, svc_callback, token),
      deadline);
  if (e != ESP_OK)
    return e;
  matches = 0;
  value_handle = 0;
  char_end = service_end;
  token = begin_operation(deadline);
  if (!token)
    return ESP_ERR_INVALID_STATE;
  e = wait_operation(ble_gattc_disc_all_chrs(conn, service_start, service_end,
                                             chr_callback, token),
                     deadline);
  if (e != ESP_OK || !ccc)
    return e;
  if (value_handle >= char_end)
    return ESP_ERR_NOT_FOUND;
  matches = 0;
  ccc_handle = 0;
  token = begin_operation(deadline);
  if (!token)
    return ESP_ERR_INVALID_STATE;
  return wait_operation(ble_gattc_disc_all_dscs(conn, value_handle, char_end,
                                                dsc_callback, token),
                        deadline);
}
static esp_err_t ble_execute_locked(const cJSON *command, uint64_t deadline,
                                    cJSON **details) {
  esp_err_t e = await_ready(deadline);
  if (e != ESP_OK)
    return e;
  const cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "parameters"),
              *timeout = cJSON_GetObjectItemCaseSensitive(params, "timeoutMs");
  const char *type = gw_action_string(command, "type"),
             *ref = gw_action_string(params, "connectionRef");
  if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 100 ||
      timeout->valuedouble > 30000 ||
      timeout->valuedouble != (uint32_t)timeout->valuedouble)
    return ESP_ERR_INVALID_ARG;
  uint64_t limit = gw_core_mono_ms() + (uint64_t)timeout->valuedouble;
  if (deadline > limit)
    deadline = limit;
  /* Only explicitly supported task and security modes may create a link. */
  bool connect_task = !strcmp(type, "GATT_CONNECT"),
       read_task = !strcmp(type, "GATT_READ"),
       write_task = !strcmp(type, "GATT_WRITE"),
       sub = !strcmp(type, "GATT_SUBSCRIBE"),
       unsub = !strcmp(type, "GATT_UNSUBSCRIBE"),
       disconnect_task = !strcmp(type, "GATT_DISCONNECT");
  if (!connect_task && !read_task && !write_task && !sub && !unsub &&
      !disconnect_task)
    return ESP_ERR_NOT_SUPPORTED;
  if (cJSON_GetObjectItemCaseSensitive(params, "autoTrigger"))
    return ESP_ERR_NOT_SUPPORTED;
  const char *security = gw_action_string(params, "security");
  bool encrypted = !strcmp(security, "BONDED_ENCRYPTED");
  if (*security && strcmp(security, "UNENCRYPTED") && !encrypted)
    return ESP_ERR_NOT_SUPPORTED;
  if (encrypted && !gw_authority_has("ESP32_BLE_GATT_CENTRAL"))
    return ESP_ERR_NOT_SUPPORTED;
  bool temporary = !*ref && (read_task || write_task);
  if ((sub || unsub || disconnect_task) && !*ref)
    return ESP_ERR_INVALID_ARG;
  uint8_t address[6];
  const char *address_type = gw_action_string(params, "addressType");
  bool addressed = connect_task || read_task || write_task;
  if (addressed &&
      (!gw_action_hex(gw_action_string(params, "deviceAddress"), address, 6) ||
       (strcmp(address_type, "PUBLIC") && strcmp(address_type, "RANDOM"))))
    return ESP_ERR_INVALID_ARG;
  if (*ref) {
    if (conn == BLE_HS_CONN_HANDLE_NONE || strcmp(ref, connection_ref) ||
        (addressed && (memcmp(address, peer, 6) ||
                       peer_type != (uint8_t)!strcmp(address_type, "RANDOM"))))
      return ESP_ERR_INVALID_STATE;
  } else {
    if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending || paused ||
        peripheral_conn != BLE_HS_CONN_HANDLE_NONE ||
        peripheral_close_pending || next_conn_id == UINT32_MAX)
      return ESP_ERR_INVALID_STATE;
    if (ble_gap_adv_active())
      return ESP_ERR_INVALID_STATE;
    (void)ble_gap_disc_cancel();
    gw_capture_radio_enabled(1, false);
    ble_addr_t target = {.type = (uint8_t)!strcmp(address_type, "RANDOM")};
    for (unsigned i = 0; i < 6; i++)
      target.val[i] = address[5 - i];
    union ble_store_key key = {0};
    union ble_store_value value = {0};
    key.sec.peer_addr = target;
    if (encrypted &&
        (!bond_store_read ||
         bond_store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &key, &value) ||
         !value.sec.ltk_present || value.sec.key_size < 7 ||
         value.sec.key_size > 16)) {
      (void)apply_scan();
      return ESP_ERR_NOT_FOUND;
    }
    mbedtls_platform_zeroize(&value, sizeof(value));
    void *token = begin_operation(deadline);
    if (!token)
      return ESP_ERR_INVALID_STATE;
    e = wait_operation(ble_gap_connect(own_type, &target,
                                       (int32_t)(deadline - gw_core_mono_ms()),
                                       NULL, gap, token),
                       deadline);
    if (e != ESP_OK) {
      ble_gap_conn_cancel();
      (void)apply_scan();
      return e;
    }
    struct ble_gap_conn_desc actual;
    if (ble_gap_conn_find(conn, &actual) ||
        memcmp(actual.peer_ota_addr.val, target.val, 6) ||
        actual.peer_ota_addr.type != target.type) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto cleanup;
    }
    conn_id = ++next_conn_id;
    memcpy(peer, address, 6);
    peer_type = target.type;
    gw_capture_connection_ref(1, boot, conn_id, connection_ref);
    cJSON *record = cJSON_CreateObject();
    char boot_hex[33], address_hex[13];
    gw_action_hex_encode(boot, 16, boot_hex);
    gw_action_hex_encode(peer, 6, address_hex);
    cJSON_AddStringToObject(record, "boot", boot_hex);
    cJSON_AddNumberToObject(record, "id", conn_id);
    cJSON_AddStringToObject(record, "address", address_hex);
    cJSON_AddStringToObject(record, "addressType", address_type);
    e = gw_store_put_json("gatt/esp-connection", record);
    cJSON_Delete(record);
    if (e != ESP_OK)
      goto cleanup;
  }
  if (encrypted) {
    struct ble_gap_conn_desc actual;
    union ble_store_key key = {0};
    union ble_store_value value = {0};
    if (ble_gap_conn_find(conn, &actual)) {
      e = ESP_ERR_INVALID_STATE;
      goto cleanup;
    }
    key.sec.peer_addr = actual.peer_id_addr;
    if (!bond_store_read ||
        bond_store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &key, &value) ||
        !value.sec.ltk_present || value.sec.key_size < 7 ||
        value.sec.key_size > 16) {
      e = ESP_ERR_NOT_FOUND;
      goto cleanup;
    }
    if (!actual.sec_state.encrypted) {
      if (!begin_operation(deadline)) {
        mbedtls_platform_zeroize(&value, sizeof(value));
        e = ESP_ERR_TIMEOUT;
        goto cleanup;
      }
      encryption_wait = true;
      int started = ble_gap_encryption_initiate(
          conn, value.sec.key_size, value.sec.ltk, value.sec.ediv,
          value.sec.rand_num, value.sec.authenticated);
      e = wait_operation(started, deadline);
      encryption_wait = false;
    }
    mbedtls_platform_zeroize(&value, sizeof(value));
    if (e != ESP_OK || ble_gap_conn_find(conn, &actual) ||
        !actual.sec_state.encrypted || actual.sec_state.key_size < 7) {
      if (e == ESP_OK)
        e = ESP_ERR_INVALID_RESPONSE;
      goto cleanup;
    }
  }
  *details = cJSON_CreateObject();
  if (!*details) {
    e = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  char address_hex[13];
  gw_action_hex_encode(peer, 6, address_hex);
  cJSON_AddStringToObject(*details, "radio", "ESP32");
  cJSON_AddStringToObject(*details, "connectionRef", connection_ref);
  cJSON_AddStringToObject(*details, "deviceAddress", address_hex);
  cJSON_AddStringToObject(*details, "addressType",
                          peer_type ? "RANDOM" : "PUBLIC");
  cJSON_AddStringToObject(*details, "connectionState", "CONNECTED");
  cJSON_AddNumberToObject(*details, "attMtu", ble_att_mtu(conn));
  if (connect_task) {
    struct ble_gap_conn_desc actual;
    if (ble_gap_conn_find(conn, &actual)) {
      e = ESP_ERR_INVALID_STATE;
      goto cleanup;
    }
    char boot_hex[33];
    gw_action_hex_encode(boot, 16, boot_hex);
    cJSON_AddStringToObject(*details, "controllerBootId", boot_hex);
    cJSON_AddNumberToObject(*details, "connectionId", conn_id);
    cJSON_AddStringToObject(*details, "securityState",
                            actual.sec_state.encrypted
                                ? (actual.sec_state.authenticated
                                       ? "AUTHENTICATED_ENCRYPTED"
                                       : "ENCRYPTED")
                                : "UNENCRYPTED");
    e = ESP_OK;
    goto done;
  }
  if (disconnect_task) {
    e = disconnect(deadline);
    if (e == ESP_OK) {
      cJSON_ReplaceItemInObjectCaseSensitive(*details, "connectionState",
                                             cJSON_CreateString("CLOSED"));
      (void)apply_scan();
    }
    goto done;
  }
  if ((e = discover(params, deadline, sub || unsub)) != ESP_OK)
    goto cleanup;
  cJSON_AddStringToObject(*details, "serviceUuid",
                          gw_action_string(params, "serviceUuid"));
  cJSON_AddStringToObject(*details, "characteristicUuid",
                          gw_action_string(params, "characteristicUuid"));
  cJSON_AddNumberToObject(*details, "characteristicHandle", value_handle);
  if (read_task) {
    if (!(properties & 0x02)) {
      e = ESP_ERR_NOT_SUPPORTED;
      goto cleanup;
    }
    read_length = 0;
    void *token = begin_operation(deadline);
    if (!token) {
      e = ESP_ERR_INVALID_STATE;
      goto cleanup;
    }
    e = wait_operation(
        ble_gattc_read_long(conn, value_handle, 0, read_callback, token),
        deadline);
    if (e == ESP_OK)
      add_base64(*details, "valueBase64", read_value, read_length);
  } else if (write_task) {
    const char *mode = gw_action_string(params, "writeMode"),
               *verify = gw_action_string(params, "verification"),
               *value = gw_action_string(params, "valueBase64");
    uint8_t bytes[512];
    size_t n = 0;
    if (((*mode && strcmp(mode, "WITH_RESPONSE")) ||
         (*verify && strcmp(verify, "NONE") && strcmp(verify, "READBACK"))) ||
        !(properties & 0x08) ||
        mbedtls_base64_decode(bytes, sizeof(bytes), &n,
                              (const unsigned char *)value, strlen(value)) ||
        !n || n > (size_t)ble_att_mtu(conn) - 3) {
      e = ESP_ERR_INVALID_ARG;
      goto cleanup;
    }
    void *token = begin_operation(deadline);
    if (!token) {
      e = ESP_ERR_INVALID_STATE;
      goto cleanup;
    }
    int started = ble_gattc_write_flat(conn, value_handle, bytes, (uint16_t)n,
                                       write_callback, token);
    e = wait_operation(started, deadline);
    if (!started && e != ESP_OK &&
        (operation_status < BLE_HS_ERR_ATT_BASE ||
         operation_status >= BLE_HS_ERR_ATT_BASE + 256))
      e = ESP_ERR_TIMEOUT;
    if (e == ESP_OK) {
      cJSON_AddBoolToObject(*details, "attResponse", true);
      if (!strcmp(verify, "READBACK")) {
        read_length = 0;
        token = begin_operation(deadline);
        e = token ? wait_operation(ble_gattc_read_long(conn, value_handle, 0,
                                                       read_callback, token),
                                   deadline)
                  : ESP_ERR_INVALID_STATE;
        if (e == ESP_OK) {
          add_base64(*details, "readbackBase64", read_value, read_length);
          if (read_length != n || memcmp(bytes, read_value, n))
            e = ESP_ERR_INVALID_RESPONSE;
        }
      }
    }
  } else {
    const char *mode = gw_action_string(params, "mode");
    uint16_t value = unsub                       ? 0
                     : !strcmp(mode, "NOTIFY")   ? 1
                     : !strcmp(mode, "INDICATE") ? 2
                                                 : 0;
    if (!unsub && (!value || !(properties & (value == 1 ? 0x10 : 0x20)))) {
      e = ESP_ERR_NOT_SUPPORTED;
      goto cleanup;
    }
    uint8_t bytes[2];
    gw_put_u16(bytes, value);
    void *token = begin_operation(deadline);
    int started = token ? ble_gattc_write_flat(conn, ccc_handle, bytes, 2,
                                               write_callback, token)
                        : BLE_HS_EINVAL;
    e = wait_operation(started, deadline);
    if (!started && e != ESP_OK &&
        (operation_status < BLE_HS_ERR_ATT_BASE ||
         operation_status >= BLE_HS_ERR_ATT_BASE + 256))
      e = ESP_ERR_TIMEOUT;
    if (e == ESP_OK) {
      cJSON_AddBoolToObject(*details, "cccEnabled", !unsub);
      cJSON_AddNumberToObject(*details, "cccHandle", ccc_handle);
      if (sub)
        cJSON_AddStringToObject(*details, "mode", mode);
    }
  }
cleanup:
  if (temporary || e != ESP_OK) {
    uint64_t close_deadline = e == ESP_OK ? deadline : gw_core_mono_ms() + 2000;
    esp_err_t closed = disconnect(close_deadline);
    if (closed != ESP_OK) {
      e = ESP_ERR_TIMEOUT;
      closed = disconnect(gw_core_mono_ms() + 2000);
      if (closed != ESP_OK)
        fault = true;
    }
    if (closed == ESP_OK) {
      if (*details)
        cJSON_ReplaceItemInObjectCaseSensitive(*details, "connectionState",
                                               cJSON_CreateString("CLOSED"));
      (void)apply_scan();
    }
    if (e == ESP_OK && gw_core_mono_ms() > deadline)
      e = ESP_ERR_TIMEOUT;
  }
done:
  if (e == ESP_OK && gw_core_mono_ms() > deadline)
    e = ESP_ERR_TIMEOUT;
  return e;
}

/* The action journal owns semantic idempotence; this lock also protects radio
 * recovery performed by the capture worker after an inbound link closes. */
static bool take_radio(uint64_t deadline) {
  uint64_t now = gw_core_mono_ms();
  return radio_lock && deadline > now &&
         xSemaphoreTake(radio_lock, pdMS_TO_TICKS(deadline - now)) == pdTRUE;
}
esp_err_t gw_ble_configure(const cJSON *o, bool adv, uint64_t revision,
                           uint64_t deadline, cJSON **details) {
  if (!take_radio(deadline))
    return ESP_ERR_TIMEOUT;
  esp_err_t e = ble_configure_locked(o, adv, revision, deadline, details);
  xSemaphoreGive(radio_lock);
  return e;
}
esp_err_t gw_ble_execute(const cJSON *command, uint64_t deadline,
                         cJSON **details) {
  if (!take_radio(deadline))
    return ESP_ERR_TIMEOUT;
  esp_err_t e = ble_execute_locked(command, deadline, details);
  xSemaphoreGive(radio_lock);
  return e;
}
esp_err_t gw_ble_pause(uint64_t deadline) {
  if (!take_radio(deadline))
    return ESP_ERR_TIMEOUT;
  esp_err_t e = ble_pause_locked(deadline);
  xSemaphoreGive(radio_lock);
  return e;
}
esp_err_t gw_ble_restore(uint64_t deadline) {
  if (!take_radio(deadline))
    return ESP_ERR_TIMEOUT;
  esp_err_t e = ble_restore_locked(deadline);
  xSemaphoreGive(radio_lock);
  return e;
}
