#ifndef GW_PERIPHERAL_H
#define GW_PERIPHERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PERIPHERAL_SERVICE_UUID "7be00001-4b5a-4c31-9d91-6c2f9a0b5400"
#define GW_PERIPHERAL_INFO_VERSION 1U
/* INFO has its own capability namespace, independent of UART HELLO. */
#define GW_PERIPHERAL_CAP_RX_WRITE (1U << 0)
#define GW_PERIPHERAL_CAP_TX_NOTIFY (1U << 1)
#define GW_PERIPHERAL_CAP_TX_DEDUP (1U << 2)

/* Register callbacks after Bluetooth/settings initialization, before advertising.
 * The static service itself is registered by BT_GATT_SERVICE_DEFINE. */
int gw_peripheral_init(void);
void gw_peripheral_poll(void);
bool gw_peripheral_busy(void);
/* Close admission before stopping connectable advertising for maintenance.
 * An already arriving connection is retained and closed through normal poll /
 * disconnect / recycle processing. Reopening does not cancel that closure. */
void gw_peripheral_set_admission(bool allowed);
/* out has room for 16 bytes; out_len is output-only. */
uint8_t gw_peripheral_command(uint8_t opcode, const uint8_t *args, size_t len,
                             uint8_t *out, size_t *out_len);

/* The queue copies the record before returning. false rejects the ATT write
 * with Insufficient Resources; receipt is never silently acknowledged. */
bool gw_app_record(uint8_t type, const uint8_t *record, size_t len);
/* Called after releasing this module's mutex; implementation must not block. */
void gw_radio_peripheral_changed(void);
/* A successful connection has ended the one-shot connectable advertiser.
 * Preserve this edge even if disconnect and recycle occur before radio poll. */
void gw_radio_peripheral_connected(void);

#endif
