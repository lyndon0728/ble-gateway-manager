#ifndef GW_GATT_H
#define GW_GATT_H

#include <stddef.h>
#include <stdint.h>

/* Initialize after bt_enable()/settings_load(). The record callback must copy
 * its input before returning and must never block the Bluetooth RX thread. */
int gw_gatt_init(void (*record_cb)(const uint8_t *, size_t));
/* out has room for at least 526 bytes; out_len is output-only. */
uint8_t gw_gatt_command(uint8_t opcode, const uint8_t *args, size_t len,
                       uint8_t *out, size_t *out_len);
/* Call regularly from the application thread, including while UART is down. */
void gw_gatt_poll(void);
uint32_t gw_gatt_connection_id(void);
uint8_t gw_gatt_connection_state(void);
void gw_gatt_connection_snapshot(uint32_t *id, uint8_t *state);

/* Radio owner pauses scan before initiation, rejects incompatible advertising,
 * and restores the permitted role configuration after initiation finishes. */
int gw_radio_prepare_connect(void);
void gw_radio_connect_complete(void);

#endif
