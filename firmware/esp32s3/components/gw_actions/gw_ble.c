#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_actions_internal.h"
#include "gw_capture.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_BT_NIMBLE_SECURITY_ENABLE) && CONFIG_BT_NIMBLE_SECURITY_ENABLE
#error "ESP BLE bond provisioning is not implemented; keep NimBLE SMP disabled"
#endif

typedef struct {
    uint8_t kind;
    uint16_t size;
    uint8_t data[536];
} report_t;
typedef struct {
    bool enabled;
    uint8_t mode;
    uint16_t interval;
    uint8_t data[31], response[31];
    size_t length, response_length;
} adv_t;
static QueueHandle_t reports;
static SemaphoreHandle_t filter_lock, done;
static struct gw_profile profile;
static adv_t advertising;
static bool ready, paused, fault;
static uint8_t own_type, boot[16], peer[6], peer_type;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t conn_id, next_conn_id, operation;
static char connection_ref[33];
static volatile bool waiting;
static bool disconnect_wait;
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
    if (now >= deadline || xSemaphoreTake(done, pdMS_TO_TICKS(deadline - now)) != pdTRUE) {
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
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &own_type) == 0)
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
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_NON,
                                   .disc_mode = advertising.mode ? BLE_GAP_DISC_MODE_GEN
                                                                 : BLE_GAP_DISC_MODE_NON,
                                   .itvl_min = advertising.interval,
                                   .itvl_max = advertising.interval,
                                   .channel_map = 7};
    complete(token, ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &p, gap, NULL));
}
static esp_err_t apply_advertising(uint64_t deadline) {
    if (fault || gw_core_mono_ms() >= deadline)
        return ESP_ERR_INVALID_STATE;
    int result = ble_gap_adv_stop();
    if (result && result != BLE_HS_EALREADY)
        return rc(result);
    if (!advertising.enabled || paused)
        return ESP_OK;
    if ((result = ble_gap_adv_set_data(advertising.data, (int)advertising.length)) != 0)
        return rc(result);
    if ((result =
             ble_gap_adv_rsp_set_data(advertising.response, (int)advertising.response_length)) != 0)
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
    int result = ble_gap_disc_cancel();
    if (result && result != BLE_HS_EALREADY)
        return rc(result);
    bool enabled = (profile.scan_flags & 1) && !paused && conn == BLE_HS_CONN_HANDLE_NONE;
    if (!enabled) {
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
    bool allowed = gw_filter_match(&profile, kind, rssi, d->data, d->length_data);
    xSemaphoreGive(filter_lock);
    if (!allowed) {
        filtered_reports++;
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
    if (xQueueSend(reports, &r, 0) != pdTRUE)
        dropped_reports++;
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
        if (!waiting || token != operation) {
            ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        conn = e->connect.conn_handle;
        complete(token, 0);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
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
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (conn != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        break;
    default:
        break;
    }
    return 0;
}
static void report_worker(void *arg) {
    (void)arg;
    report_t r;
    for (;;) {
        if (xQueueReceive(reports, &r, pdMS_TO_TICKS(50)) == pdTRUE) {
            esp_err_t e =
                r.kind ? gw_capture_esp_gatt(boot, r.data, r.size) : gw_capture_esp(r.data, r.size);
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
        if (pending && gw_capture_link_down(1, boot, gw_get_u32(r.data), r.data[4], r.data + 5,
                                            "DISCONNECTED") == ESP_OK) {
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
    return ready && !fault && gw_core_mono_ms() < deadline ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t gw_ble_init(void) {
    gw_capture_esp_boot(boot);
    gw_profile_default(&profile);
    filter_lock = xSemaphoreCreateMutex();
    done = xSemaphoreCreateBinary();
    reports = xQueueCreate(32, sizeof(report_t));
    if (!filter_lock || !done || !reports)
        return ESP_ERR_NO_MEM;
    cJSON *old = gw_store_get_json("gatt/esp-connection");
    if (old) {
        uint8_t old_boot[16], address[6];
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(old, "id");
        if (!gw_action_hex(gw_action_string(old, "boot"), old_boot, 16) ||
            !gw_action_hex(gw_action_string(old, "address"), address, 6) || !cJSON_IsNumber(id) ||
            gw_capture_link_down(1, old_boot, (uint32_t)id->valuedouble,
                                 !strcmp(gw_action_string(old, "addressType"), "RANDOM"), address,
                                 "ESP_RESTART") != ESP_OK) {
            cJSON_Delete(old);
            return ESP_FAIL;
        }
        cJSON_Delete(old);
        if (gw_store_delete("gatt/esp-connection") != ESP_OK)
            return ESP_FAIL;
    }
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK)
        return e;
    ble_hs_cfg.sync_cb = synchronized;
    ble_hs_cfg.reset_cb = reset;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    if (xTaskCreate(report_worker, "esp_ble_capture", 6144, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    nimble_port_freertos_init(host);
    return ESP_OK;
}
esp_err_t gw_ble_configure(const cJSON *o, bool adv, uint64_t revision, uint64_t deadline,
                           cJSON **details) {
    esp_err_t e = await_ready(deadline);
    if (e != ESP_OK)
        return e;
    if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending || paused)
        return ESP_ERR_INVALID_STATE;
    if (adv) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, "advertising");
        const char *mode = gw_action_string(a, "mode");
        adv_t next = {0};
        if (!strcmp(mode, "SCANNABLE_NONCONNECTABLE"))
            next.mode = 1;
        else if (strcmp(mode, "NONCONNECTABLE"))
            return ESP_ERR_NOT_SUPPORTED;
        const cJSON *interval = cJSON_GetObjectItemCaseSensitive(a, "intervalMs"),
                    *enabled = cJSON_GetObjectItemCaseSensitive(a, "enabled");
        if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(interval) || interval->valuedouble < 100 ||
            interval->valuedouble > 10240 || (int)interval->valuedouble != interval->valuedouble)
            return ESP_ERR_INVALID_ARG;
        next.enabled = cJSON_IsTrue(enabled);
        next.interval = (uint16_t)(((int)interval->valuedouble * 8 + 2) / 5);
        const char *data = gw_action_string(a, "advDataBase64"),
                   *response = gw_action_string(a, "scanResponseBase64");
        if (mbedtls_base64_decode(next.data, 31, &next.length, (const unsigned char *)data,
                                  strlen(data)) ||
            mbedtls_base64_decode(next.response, 31, &next.response_length,
                                  (const unsigned char *)response, strlen(response)) ||
            !gw_ad_valid(next.data, next.length) ||
            !gw_ad_valid(next.response, next.response_length) ||
            (!next.mode && next.response_length))
            return ESP_ERR_INVALID_ARG;
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
        struct gw_profile *next = malloc(sizeof(*next));
        if (!next)
            return ESP_ERR_NO_MEM;
        e = gw_action_profile(o, revision, next);
        if (e != ESP_OK) {
            free(next);
            return e;
        }
        xSemaphoreTake(filter_lock, portMAX_DELAY);
        struct gw_profile previous = profile;
        profile = *next;
        xSemaphoreGive(filter_lock);
        free(next);
        e = apply_scan();
        if (e != ESP_OK) {
            xSemaphoreTake(filter_lock, portMAX_DELAY);
            profile = previous;
            xSemaphoreGive(filter_lock);
            if (apply_scan() != ESP_OK)
                fault = true;
            return e;
        }
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
    cJSON_AddNumberToObject(j, "droppedReports", dropped_reports);
    cJSON_AddNumberToObject(j, "droppedNotifications", dropped_notifications);
    cJSON_AddNumberToObject(j, "filteredReports", filtered_reports);
    cJSON_AddStringToObject(j, "connectionRef", connection_ref);
    return j;
}
esp_err_t gw_ble_pause(uint64_t deadline) {
    if (!ready)
        return ESP_OK;
    if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending)
        return ESP_ERR_INVALID_STATE;
    paused = true;
    esp_err_t e = apply_scan();
    if (e == ESP_OK)
        e = apply_advertising(deadline);
    return e;
}
esp_err_t gw_ble_restore(uint64_t deadline) {
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
    complete(token,
             error->status == BLE_HS_EDONE ? (matches == 1 ? 0 : BLE_HS_ENOENT) : error->status);
    return 0;
}
static int chr_callback(uint16_t handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg) {
    (void)handle;
    uint32_t token = (uint32_t)(uintptr_t)arg;
    if (!waiting || operation != token)
        return 0;
    if (!error->status && chr) {
        if (value_handle && chr->def_handle > value_handle && chr->def_handle <= char_end)
            char_end = chr->def_handle - 1;
        if (!ble_uuid_cmp(&chr->uuid.u, &target_uuid.u)) {
            matches++;
            value_handle = chr->val_handle;
            properties = chr->properties;
        }
        return 0;
    }
    complete(token,
             error->status == BLE_HS_EDONE ? (matches == 1 ? 0 : BLE_HS_ENOENT) : error->status);
    return 0;
}
static int dsc_callback(uint16_t handle, const struct ble_gatt_error *error, uint16_t char_handle,
                        const struct ble_gatt_dsc *dsc, void *arg) {
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
    complete(token,
             error->status == BLE_HS_EDONE ? (matches == 1 ? 0 : BLE_HS_ENOENT) : error->status);
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
    esp_err_t e = wait_operation(ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM), deadline);
    disconnect_wait = false;
    return e == ESP_OK && conn != BLE_HS_CONN_HANDLE_NONE ? ESP_ERR_TIMEOUT : e;
}
static void add_base64(cJSON *o, const char *key, const uint8_t *value, size_t n) {
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
    esp_err_t e =
        wait_operation(ble_gattc_disc_svc_by_uuid(conn, &service.u, svc_callback, token), deadline);
    if (e != ESP_OK)
        return e;
    matches = 0;
    value_handle = 0;
    char_end = service_end;
    token = begin_operation(deadline);
    if (!token)
        return ESP_ERR_INVALID_STATE;
    e = wait_operation(
        ble_gattc_disc_all_chrs(conn, service_start, service_end, chr_callback, token), deadline);
    if (e != ESP_OK || !ccc)
        return e;
    if (value_handle >= char_end)
        return ESP_ERR_NOT_FOUND;
    matches = 0;
    ccc_handle = 0;
    token = begin_operation(deadline);
    if (!token)
        return ESP_ERR_INVALID_STATE;
    return wait_operation(
        ble_gattc_disc_all_dscs(conn, value_handle, char_end, dsc_callback, token), deadline);
}
esp_err_t gw_ble_execute(const cJSON *command, uint64_t deadline, cJSON **details) {
    esp_err_t e = await_ready(deadline);
    if (e != ESP_OK)
        return e;
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(command, "parameters"),
                *timeout = cJSON_GetObjectItemCaseSensitive(params, "timeoutMs");
    const char *type = gw_action_string(command, "type"),
               *ref = gw_action_string(params, "connectionRef");
    if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 100 || timeout->valuedouble > 30000 ||
        timeout->valuedouble != (uint32_t)timeout->valuedouble)
        return ESP_ERR_INVALID_ARG;
    uint64_t limit = gw_core_mono_ms() + (uint64_t)timeout->valuedouble;
    if (deadline > limit)
        deadline = limit;
    /* Only explicitly supported task and security modes may create a link. */
    bool connect_task = !strcmp(type, "GATT_CONNECT"), read_task = !strcmp(type, "GATT_READ"),
         write_task = !strcmp(type, "GATT_WRITE"), sub = !strcmp(type, "GATT_SUBSCRIBE"),
         unsub = !strcmp(type, "GATT_UNSUBSCRIBE"),
         disconnect_task = !strcmp(type, "GATT_DISCONNECT");
    if (!connect_task && !read_task && !write_task && !sub && !unsub && !disconnect_task)
        return ESP_ERR_NOT_SUPPORTED;
    if (cJSON_GetObjectItemCaseSensitive(params, "autoTrigger"))
        return ESP_ERR_NOT_SUPPORTED;
    const char *security = gw_action_string(params, "security");
    if (*security && strcmp(security, "UNENCRYPTED"))
        return ESP_ERR_NOT_SUPPORTED;
    bool temporary = !*ref && (read_task || write_task);
    if ((sub || unsub || disconnect_task) && !*ref)
        return ESP_ERR_INVALID_ARG;
    uint8_t address[6];
    const char *address_type = gw_action_string(params, "addressType");
    bool addressed = connect_task || read_task || write_task;
    if (addressed && (!gw_action_hex(gw_action_string(params, "deviceAddress"), address, 6) ||
                      (strcmp(address_type, "PUBLIC") && strcmp(address_type, "RANDOM"))))
        return ESP_ERR_INVALID_ARG;
    if (*ref) {
        if (conn == BLE_HS_CONN_HANDLE_NONE || strcmp(ref, connection_ref) ||
            (addressed &&
             (memcmp(address, peer, 6) || peer_type != (uint8_t)!strcmp(address_type, "RANDOM"))))
            return ESP_ERR_INVALID_STATE;
    } else {
        if (conn != BLE_HS_CONN_HANDLE_NONE || close_pending || paused ||
            next_conn_id == UINT32_MAX)
            return ESP_ERR_INVALID_STATE;
        if (ble_gap_adv_active())
            return ESP_ERR_INVALID_STATE;
        (void)ble_gap_disc_cancel();
        gw_capture_radio_enabled(1, false);
        ble_addr_t target = {.type = (uint8_t)!strcmp(address_type, "RANDOM")};
        for (unsigned i = 0; i < 6; i++)
            target.val[i] = address[5 - i];
        void *token = begin_operation(deadline);
        if (!token)
            return ESP_ERR_INVALID_STATE;
        e = wait_operation(ble_gap_connect(own_type, &target,
                                           (int32_t)(deadline - gw_core_mono_ms()), NULL, gap,
                                           token),
                           deadline);
        if (e != ESP_OK) {
            ble_gap_conn_cancel();
            (void)apply_scan();
            return e;
        }
        struct ble_gap_conn_desc actual;
        if (ble_gap_conn_find(conn, &actual) || memcmp(actual.peer_ota_addr.val, target.val, 6) ||
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
    cJSON_AddStringToObject(*details, "addressType", peer_type ? "RANDOM" : "PUBLIC");
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
        cJSON_AddStringToObject(
            *details, "securityState",
            actual.sec_state.encrypted
                ? (actual.sec_state.authenticated ? "AUTHENTICATED_ENCRYPTED" : "ENCRYPTED")
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
    cJSON_AddStringToObject(*details, "serviceUuid", gw_action_string(params, "serviceUuid"));
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
        e = wait_operation(ble_gattc_read_long(conn, value_handle, 0, read_callback, token),
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
            mbedtls_base64_decode(bytes, sizeof(bytes), &n, (const unsigned char *)value,
                                  strlen(value)) ||
            !n || n > (size_t)ble_att_mtu(conn) - 3) {
            e = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        void *token = begin_operation(deadline);
        if (!token) {
            e = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        int started =
            ble_gattc_write_flat(conn, value_handle, bytes, (uint16_t)n, write_callback, token);
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
                e = token ? wait_operation(
                                ble_gattc_read_long(conn, value_handle, 0, read_callback, token),
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
        int started = token
                          ? ble_gattc_write_flat(conn, ccc_handle, bytes, 2, write_callback, token)
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
