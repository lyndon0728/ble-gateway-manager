#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
typedef struct {const char *key;const void *data;size_t size;bool remove;} gw_store_op_t;
uint64_t gw_core_mono_ms(void);
esp_err_t gw_store_transaction(const gw_store_op_t *,size_t);
esp_err_t gw_store_put_json(const char *,const cJSON *);
cJSON *gw_store_get_json(const char *);
esp_err_t gw_core_maintenance_acquire(const char *);
void gw_core_maintenance_release(const char *);
esp_err_t gw_factory_get_string(const char *,char *,size_t);
bool gw_core_time_trusted(void);
uint64_t gw_core_time_deadline(uint64_t expires);
bool gw_core_interface_ready(const char *);
esp_err_t gw_core_wifi_scan(bool,uint64_t,cJSON **);
cJSON *gw_core_state(void);
