#ifndef GW_RADIO_H
#define GW_RADIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int gw_radio_init(void);
uint8_t gw_radio_command(uint8_t opcode, const uint8_t *args, size_t length,
                         uint8_t *result, size_t *result_length);
int gw_radio_prepare_connect(void);
void gw_radio_connect_complete(void);
void gw_radio_peripheral_changed(void);
void gw_radio_peripheral_connected(void);
int gw_radio_enter_maintenance(void);
void gw_radio_leave_maintenance(void);
int gw_radio_prepare_dfu(void);
void gw_radio_finish_dfu(void);
void gw_radio_poll(void);
uint32_t gw_radio_capabilities(void);
uint64_t gw_radio_revision(void);
uint8_t gw_radio_scan_state(void);
void gw_radio_counters(uint64_t *seen, uint64_t *filtered, uint64_t *incomplete);
bool gw_app_record(uint8_t type, const uint8_t *record, size_t length);
#endif
