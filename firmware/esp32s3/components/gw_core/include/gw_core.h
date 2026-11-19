#ifndef GW_ESP_CORE_H
#define GW_ESP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const void *data;
    size_t size;
    bool remove;
} gw_store_op_t;

/* All ops are published by one durable snapshot pointer. Caller owns inputs. */
esp_err_t gw_store_transaction(const gw_store_op_t *ops, size_t count);
esp_err_t gw_store_get(const char *key, void *data, size_t *size);
esp_err_t gw_store_put(const char *key, const void *data, size_t size);
esp_err_t gw_store_delete(const char *key);
esp_err_t gw_store_put_json(const char *key, const cJSON *value);
cJSON *gw_store_get_json(const char *key); /* caller cJSON_Delete */

/* Factory NVS is encrypted and never reset by normal recovery. */
esp_err_t gw_factory_get_string(const char *name, char *out, size_t size);
esp_err_t gw_factory_get_blob(const char *name, void *out, size_t *size);

typedef struct {
    bool remote;
    char transaction_id[65];
    char task_id[129];
    char desired_revision[24];
    char snapshot_id[65];
    uint64_t network_generation;
    uint64_t deadline_mono_ms;
    const cJSON *network; /* immutable, valid only during callback */
    const char *interface; /* WIFI_STA or ETHERNET */
} gw_network_probe_t;
/* Executed on network worker without state mutex. Must honor deadline.
 * Remote callback succeeds only after durable RESULT_PERSISTED; local callback
 * succeeds only after matching authenticated challenge response. */
typedef esp_err_t (*gw_network_probe_fn)(const gw_network_probe_t *, void *ctx);

esp_err_t gw_core_init(void);
void gw_core_set_probe_handler(gw_network_probe_fn callback, void *ctx);
uint64_t gw_core_mono_ms(void);
void gw_core_set_local_window_deadline(uint64_t deadline_mono_ms);
esp_err_t gw_core_dispatch(const cJSON *request, cJSON **response);
/* response and gw_core_transaction/state results are caller-owned. */
esp_err_t gw_core_remote_begin(const char *task_id, const cJSON *network,
    const char *desired_revision, const cJSON *precondition,
    uint64_t deadline_mono_ms, cJSON **response);
cJSON *gw_core_transaction(const char *transaction_id);
cJSON *gw_core_transaction_by_task(const char *task_id);
/* Call only after durable cloud ACK of SUCCEEDED for this configuration task. */
esp_err_t gw_core_platform_ack(const char *task_id);
cJSON *gw_core_state(void);
esp_err_t gw_core_network_snapshot_id(const cJSON *network, char hex[65]);
esp_err_t gw_core_credential_install(const char *ref, const char *version,
    const char *ssid, const char *security, const char *password,
    const char *delivery_id);
cJSON *gw_core_credential_metadata(void);
esp_err_t gw_core_maintenance_acquire(const char *owner);
void gw_core_maintenance_release(const char *owner);

esp_err_t gw_core_ap_start(void);
esp_err_t gw_core_ap_stop(void);
esp_netif_t *gw_core_ap_netif(void);
esp_netif_t *gw_core_uplink_netif(const char *interface);
bool gw_core_interface_ready(const char *interface);
bool gw_core_time_trusted(void);
bool gw_core_time_bounds(uint64_t *lower_utc_ms, uint64_t *upper_utc_ms);
uint64_t gw_core_time_deadline(uint64_t expires_utc_ms);
/* Only the nonce-bound signed-time verifier may call this entry point. */
esp_err_t gw_core_time_accept(uint64_t issued_ms, uint32_t uncertainty_ms,
                              uint64_t sent_mono_ms, uint64_t received_mono_ms);
/* Bounded real Wi-Fi scan for an already authorized diagnostic caller.
 * Uses the same network ownership lease as provisioning/candidates. */
esp_err_t gw_core_wifi_scan(bool active, uint64_t deadline_mono_ms, cJSON **aps);

#ifdef __cplusplus
}
#endif
#endif
