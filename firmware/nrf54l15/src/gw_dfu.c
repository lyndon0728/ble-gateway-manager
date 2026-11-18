#include "gw_dfu.h"
#include "gw_core.h"
#include <errno.h>
#include <string.h>

#if defined(CONFIG_GW_DFU)
#include <zephyr/kernel.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Offset-swap is the only reviewed layout. Never silently fall back to an
 * overwrite mode (which could not revert) or to the running image slot. */
#if !defined(CONFIG_BOOTLOADER_MCUBOOT) || \
    !defined(CONFIG_MCUBOOT_BOOTLOADER_MODE_SWAP_USING_OFFSET)
#error "GW_DFU requires MCUboot swap-using-offset and two signed image slots"
#endif

enum { DFU_IDLE, DFU_RECEIVING, DFU_READY, DFU_FAULT, DFU_ABORTED, DFU_REBOOT_PENDING };
static struct {
    bool available, confirmed, verified_running;
    uint8_t state;
    uint64_t session;
    uint32_t image_size, received, maximum, last_offset;
    uint16_t last_length;
    int32_t last_error;
    uint8_t digest[32], last_chunk[GW_DFU_CHUNK_MAX];
    const struct flash_area *area;
    struct flash_img_context writer;
    int64_t confirm_deadline, reboot_deadline;
} dfu;

static void close_writer(void)
{
    if (dfu.writer.flash_area) {
        flash_area_close(dfu.writer.flash_area);
        dfu.writer.flash_area = NULL;
    }
}

static uint8_t failed(int error)
{
    dfu.last_error = error;
    dfu.state = DFU_FAULT;
    close_writer();
    return GW_APPLY_FAILED;
}

static void status_bytes(uint8_t *out, size_t *length)
{
    memset(out, 0, GW_DFU_STATUS_SIZE);
    gw_put_u64(out, dfu.session);
    out[8] = dfu.state;
    out[9] = dfu.confirmed;
    gw_put_u32(out + 10, dfu.received);
    gw_put_u32(out + 14, dfu.image_size);
    gw_put_u32(out + 18, dfu.maximum);
    gw_put_u32(out + 22, (uint32_t)dfu.last_error);
    memcpy(out + 26, dfu.digest, 32);
    int64_t remaining = dfu.confirm_deadline - k_uptime_get();
    if (!dfu.confirmed && remaining > 0) {
        gw_put_u32(out + 58, (uint32_t)remaining);
    }
    *length = GW_DFU_STATUS_SIZE;
}

int gw_dfu_init(void)
{
    const struct flash_area *primary;
    struct flash_sector first_sector;
    uint32_t sector_count = 1;
    memset(&dfu, 0, sizeof(dfu));
    dfu.confirmed = boot_is_img_confirmed();
    if (!dfu.confirmed) {
        dfu.confirm_deadline = k_uptime_get() + CONFIG_GW_DFU_CONFIRM_TIMEOUT_MS;
    }
    if (flash_img_get_upload_slot() != FIXED_PARTITION_ID(slot1_partition) ||
        boot_fetch_active_slot() != FIXED_PARTITION_ID(slot0_partition)) {
        return -ENOTSUP;
    }
    int err = flash_area_open(FIXED_PARTITION_ID(slot1_partition), &dfu.area);
    if (err) { return err; }
    err = flash_area_open(FIXED_PARTITION_ID(slot0_partition), &primary);
    if (err) { return err; }
    uint32_t smaller = primary->fa_size < dfu.area->fa_size ? primary->fa_size : dfu.area->fa_size;
    bool disjoint = flash_area_get_device(primary) == flash_area_get_device(dfu.area) &&
                    (uint64_t)primary->fa_off + primary->fa_size <= (uint64_t)dfu.area->fa_off;
    flash_area_close(primary);
    /* Reserve includes offset-swap's extra sector and MCUboot status/trailer.
     * The shipped nRF54L15 DTS has 664 KiB per slot: report <=600 KiB. */
    const uint32_t reserve = 65536U;
    err = flash_area_sectors(dfu.area, &sector_count, &first_sector);
    if (!disjoint || (err && err != -ENOMEM) || sector_count != 1 ||
        first_sector.fs_size < 4096 || first_sector.fs_size > reserve / 2U ||
        smaller <= reserve) {
        return -ENOTSUP;
    }
    ssize_t trailer = boot_get_trailer_status_offset(dfu.area->fa_size);
    if (trailer < 0 || (size_t)trailer < smaller - reserve + first_sector.fs_size) {
        return -ENOSPC;
    }
    dfu.maximum = smaller - reserve;
    dfu.available = true;
    return 0;
}

bool gw_dfu_available(void) { return dfu.available; }
bool gw_dfu_active(void)
{
    return dfu.state == DFU_RECEIVING || dfu.state == DFU_READY ||
           dfu.state == DFU_FAULT || dfu.state == DFU_REBOOT_PENDING;
}

static uint8_t begin(const uint8_t *args, size_t length)
{
    if (length != 44) { return GW_BAD_PARAM; }
    uint64_t session = gw_get_u64(args);
    uint32_t size = gw_get_u32(args + 8);
    if (!session || size < 32 || size > dfu.maximum) { return GW_BAD_PARAM; }
    /* Never erase the old image while it is the revert candidate for a test
     * boot. ESP must confirm this running image before starting another DFU. */
    if (!dfu.confirmed) { return GW_BUSY; }
    if (session == dfu.session) {
        if (size != dfu.image_size || memcmp(args + 12, dfu.digest, 32)) { return GW_CONFLICT; }
        if (dfu.state == DFU_ABORTED) { return GW_STALE; }
        return dfu.state == DFU_FAULT ? GW_APPLY_FAILED : GW_OK;
    }
    if (gw_dfu_active()) { return GW_BUSY; }
    int err = gw_radio_prepare_dfu();
    if (err) { return err == -EBUSY ? GW_BUSY : GW_APPLY_FAILED; }
    dfu.session = session;
    dfu.image_size = size;
    dfu.received = 0;
    dfu.last_length = 0;
    dfu.last_error = 0;
    dfu.state = DFU_RECEIVING;
    memcpy(dfu.digest, args + 12, 32);
    /* flatten handles RRAM (no explicit erase) as well as erasable devices;
     * erase only slot1, including any obsolete pending-upgrade trailer. */
    err = flash_area_flatten(dfu.area, 0, dfu.area->fa_size);
    if (err) { return failed(err); }
    memset(&dfu.writer, 0, sizeof(dfu.writer));
    err = flash_img_init_id(&dfu.writer, FIXED_PARTITION_ID(slot1_partition));
    return err ? failed(err) : GW_OK;
}

static bool supported_header(const uint8_t *data, size_t length)
{
    if (length < 32 || gw_get_u32(data) != 0x96f3b83dU) { return false; }
    uint32_t header = gw_get_u16(data + 8);
    uint32_t body = gw_get_u32(data + 12);
    /* This endpoint accepts an unencrypted, uncompressed signed MCUboot
     * application image. Signature TLVs are verified by MCUboot on reboot. */
    return header >= 32 && header <= dfu.image_size && body != 0 &&
           body <= dfu.image_size - header && gw_get_u32(data + 16) == 0;
}

static uint8_t chunk(const uint8_t *args, size_t length)
{
    if (length < 15 || gw_get_u16(args + 12) != length - 14 ||
        length - 14 > GW_DFU_CHUNK_MAX) { return GW_BAD_PARAM; }
    if (gw_get_u64(args) != dfu.session || !dfu.session) { return GW_NOT_FOUND; }
    if (dfu.state != DFU_RECEIVING) { return GW_BUSY; }
    uint32_t offset = gw_get_u32(args + 8);
    uint16_t count = gw_get_u16(args + 12);
    if (offset < dfu.received) {
        if (offset != dfu.last_offset) { return GW_STALE; }
        return count == dfu.last_length && !memcmp(args + 14, dfu.last_chunk, count)
               ? GW_OK : GW_CONFLICT;
    }
    if (offset != dfu.received) { return GW_CONFLICT; }
    if (count > dfu.image_size - dfu.received ||
        (!offset && !supported_header(args + 14, count))) { return GW_BAD_PARAM; }
    int err = flash_img_buffered_write(&dfu.writer, args + 14, count, false);
    if (err) { return failed(err); }
    dfu.last_offset = offset;
    dfu.last_length = count;
    memcpy(dfu.last_chunk, args + 14, count);
    dfu.received += count;
    return GW_OK;
}

static uint8_t finish(void)
{
    if (dfu.state == DFU_READY || dfu.state == DFU_REBOOT_PENDING) { return GW_OK; }
    if (dfu.state != DFU_RECEIVING || dfu.received != dfu.image_size) { return GW_CONFLICT; }
    int err = flash_img_buffered_write(&dfu.writer, NULL, 0, true);
    if (err) { return failed(err); }
    const struct flash_img_check check = {.match = dfu.digest, .clen = dfu.image_size};
    err = flash_img_check(&dfu.writer, &check, FIXED_PARTITION_ID(slot1_partition));
    if (err) { return failed(err); }
    struct mcuboot_img_header header;
    err = boot_read_bank_header(FIXED_PARTITION_ID(slot1_partition), &header, sizeof(header));
    if (err || header.mcuboot_version != 1 || header.h.v1.image_size > dfu.image_size) {
        return failed(err ? err : -EBADMSG);
    }
    err = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (err) { return failed(err); }
    dfu.state = DFU_READY;
    return GW_OK;
}

uint8_t gw_dfu_command(uint8_t opcode, const uint8_t *args, size_t length,
                       uint8_t *out, size_t *out_length)
{
    if (!out || !out_length || (!args && length)) { return GW_BAD_PARAM; }
    *out_length = 0;
    if (!dfu.available) { return GW_UNSUPPORTED; }
    uint8_t result;
    switch (opcode) {
    case 0x30: result = begin(args, length); break;
    case 0x31: result = chunk(args, length); break;
    case 0x33:
        if (length) { return GW_BAD_PARAM; }
        result = GW_OK;
        break;
    case 0x36:
        if (length) { return GW_BAD_PARAM; }
        if (gw_dfu_active()) { return GW_BUSY; }
        if (!dfu.verified_running) { return GW_CONFLICT; }
        if (dfu.confirmed) { return GW_OK; }
        if (k_uptime_get() >= dfu.confirm_deadline) { return GW_STALE; }
        dfu.last_error = boot_write_img_confirmed();
        if (dfu.last_error) { return GW_APPLY_FAILED; }
        dfu.confirmed = true;
        dfu.confirm_deadline = 0;
        return GW_OK;
    case 0x37: {
        if (gw_dfu_active()) { return GW_BUSY; }
        dfu.verified_running = false;
        if (length != 36 || gw_get_u32(args) < 32 || gw_get_u32(args) > dfu.maximum) {
            dfu.last_error = -EINVAL;
            return GW_BAD_PARAM;
        }
        const struct flash_img_check check = {.match = args + 4, .clen = gw_get_u32(args)};
        dfu.last_error = flash_img_check(&dfu.writer, &check, FIXED_PARTITION_ID(slot0_partition));
        if (dfu.last_error) { return GW_APPLY_FAILED; }
        dfu.verified_running = true;
        return GW_OK;
    }
    case 0x32: case 0x34: case 0x35:
        if (length != 8) { return GW_BAD_PARAM; }
        if (!dfu.session || gw_get_u64(args) != dfu.session) { return GW_NOT_FOUND; }
        if (opcode == 0x32) { result = finish(); break; }
        if (opcode == 0x35) {
            if (dfu.state != DFU_READY && dfu.state != DFU_REBOOT_PENDING) { return GW_CONFLICT; }
            if (dfu.state == DFU_READY) {
                dfu.state = DFU_REBOOT_PENDING;
                dfu.reboot_deadline = k_uptime_get() + 1000;
            }
            return GW_OK;
        }
        if (dfu.state == DFU_REBOOT_PENDING) { return GW_BUSY; }
        if (dfu.state != DFU_ABORTED) {
            close_writer();
            int err = flash_area_flatten(dfu.area, 0, dfu.area->fa_size);
            if (err) { return failed(err); }
            dfu.state = DFU_ABORTED;
            dfu.received = 0;
            dfu.last_length = 0;
            dfu.last_error = 0;
            gw_radio_finish_dfu();
        }
        result = GW_OK;
        break;
    default: return GW_UNSUPPORTED;
    }
    if (result == GW_OK) { status_bytes(out, out_length); }
    return result;
}

void gw_dfu_poll(void)
{
    /* Keep the test-boot watchdog even if slot validation disabled new DFU. */
    if (!dfu.available && dfu.confirmed) { return; }
    int64_t now = k_uptime_get();
    if ((!dfu.confirmed && now >= dfu.confirm_deadline) ||
        (dfu.state == DFU_REBOOT_PENDING && now >= dfu.reboot_deadline)) {
        sys_reboot(SYS_REBOOT_COLD);
    }
}

#else
int gw_dfu_init(void) { return 0; }
bool gw_dfu_available(void) { return false; }
bool gw_dfu_active(void) { return false; }
void gw_dfu_poll(void) {}
uint8_t gw_dfu_command(uint8_t opcode, const uint8_t *args, size_t length,
                       uint8_t *out, size_t *out_length)
{
    (void)opcode; (void)args; (void)length; (void)out;
    if (out_length) { *out_length = 0; }
    return GW_UNSUPPORTED;
}
#endif
