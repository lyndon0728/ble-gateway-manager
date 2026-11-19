#pragma once
#include "esp_err.h"
#include <stdbool.h>
/* Pinned IDF 5.5.6 NimBLE NVS record contract. Check before host init, then
 * compare restored public store records before accepting any BLE traffic. */
esp_err_t gw_ble_bonds_check(bool restored);
