#ifndef GW_DFU_H
#define GW_DFU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_DFU_STATUS_SIZE 62U
#define GW_DFU_CHUNK_MAX 1024U

int gw_dfu_init(void);
bool gw_dfu_available(void);
bool gw_dfu_active(void);
void gw_dfu_poll(void);
/* Application-thread API; out has at least GW_DFU_STATUS_SIZE bytes. */
uint8_t gw_dfu_command(uint8_t opcode, const uint8_t *args, size_t length,
                       uint8_t *out, size_t *out_length);

/* RF owner rejects active connections and preserves desired configuration. */
int gw_radio_prepare_dfu(void);
void gw_radio_finish_dfu(void);
#endif
