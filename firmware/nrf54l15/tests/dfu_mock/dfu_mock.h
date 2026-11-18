#ifndef DFU_MOCK_H
#define DFU_MOCK_H
/* Host-only storage/boot facade. Does not validate SDK ABI or real signatures. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define CONFIG_GW_DFU 1
#define CONFIG_BOOTLOADER_MCUBOOT 1
#define CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET 1
#define CONFIG_GW_DFU_CONFIRM_TIMEOUT_MS 120000
#define FIXED_PARTITION_ID(label) label
#define slot0_partition 0
#define slot1_partition 1
#define BOOT_UPGRADE_TEST 0
#define SYS_REBOOT_COLD 0
struct device { int unused; };
struct flash_area { uint8_t fa_id; uint32_t fa_off, fa_size; };
struct flash_sector { uint32_t fs_off, fs_size; };
struct flash_img_context { const struct flash_area *flash_area; uint32_t accepted; };
struct flash_img_check { const uint8_t *match; size_t clen; };
struct mcuboot_img_header { uint32_t mcuboot_version; union { struct { uint32_t image_size; } v1; } h; };
extern int64_t dfu_mock_time;
static inline int64_t k_uptime_get(void) { return dfu_mock_time; }
int flash_area_open(uint8_t id, const struct flash_area **area);
void flash_area_close(const struct flash_area *area);
const struct device *flash_area_get_device(const struct flash_area *area);
int flash_area_sectors(const struct flash_area *area, uint32_t *count, struct flash_sector *sector);
int flash_area_flatten(const struct flash_area *area, uint32_t offset, size_t length);
int flash_img_init_id(struct flash_img_context *ctx, uint8_t id);
int flash_img_buffered_write(struct flash_img_context *ctx, const uint8_t *data, size_t length, bool flush);
int flash_img_check(struct flash_img_context *ctx, const struct flash_img_check *check, uint8_t id);
uint8_t flash_img_get_upload_slot(void);
uint8_t boot_fetch_active_slot(void);
bool boot_is_img_confirmed(void);
ssize_t boot_get_trailer_status_offset(size_t area_size);
int boot_read_bank_header(uint8_t area_id, struct mcuboot_img_header *header, size_t length);
int boot_request_upgrade(int permanent);
int boot_write_img_confirmed(void);
void sys_reboot(int type);
#endif
