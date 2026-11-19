#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copies into a bounded queue; never blocks for authentication or flash.
 * data=NULL, length=0 signals disconnect; caller retries BUSY off the host.
 * ATT success is only RAM admission. Business STORED follows durable custody.
 */
typedef esp_err_t (*gw_ble_peripheral_receiver)(uint32_t connection_id,
                                                const uint8_t *data,
                                                size_t length, uint16_t mtu,
                                                void *context);
void gw_ble_peripheral_set_receiver(gw_ble_peripheral_receiver receiver,
                                    void *context);
bool gw_ble_peripheral_identity(uint32_t connection_id,
                                uint8_t controller_boot[16], uint8_t address[6],
                                uint8_t *address_type, uint8_t *security_level,
                                uint16_t *mtu);
/* Notify acceptance is not phone application delivery. Same id/seq/data is
 * idempotent, same seq/different bytes conflicts, and old seq is rejected. */
esp_err_t gw_ble_peripheral_send(uint32_t connection_id, uint64_t tx_sequence,
                                 const uint8_t *data, size_t length);
esp_err_t gw_ble_peripheral_close(uint32_t connection_id);
