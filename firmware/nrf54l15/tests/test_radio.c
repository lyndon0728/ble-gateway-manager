#include "gw_core.h"
#include "gw_radio.h"
#include "gw_gatt.h"
#include <radio_mock.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int64_t radio_mock_now = 12345;
static struct bt_le_scan_cb *registered_scan;
static unsigned int scan_starts, scan_stops, adv_starts, adv_stops;
static unsigned int fail_scan_starts, fail_adv_starts;
static unsigned int adv_updates, fail_adv_updates;
static bool peripheral_busy;
static bool stack_connection, peripheral_admission = true;
static struct bt_le_scan_param scan_params;
static uint32_t adv_options;
static bool adv_rsp_null;
static size_t adv_rsp_count;
static uint8_t connection_state;
static uint32_t connection_id;
static unsigned int record_count;
static uint8_t last_record[55];
static size_t last_record_length;
static const struct bt_bond_info bond = {.addr = {.type = 1, .a = {.val = {1,2,3,4,5,6}}}};

int bt_enable(void *callback) { (void)callback; return 0; }
int settings_load(void) { return 0; }
void bt_foreach_bond(uint8_t id, void (*callback)(const struct bt_bond_info *, void *), void *user)
{ assert(id == BT_ID_DEFAULT); callback(&bond, user); }
void bt_le_scan_cb_register(struct bt_le_scan_cb *callback) { registered_scan = callback; }
int bt_le_scan_start(const struct bt_le_scan_param *params, void *callback)
{
    (void)callback;
    ++scan_starts;
    scan_params = *params;
    if (fail_scan_starts) { --fail_scan_starts; return -EIO; }
    return 0;
}
int bt_le_scan_stop(void) { ++scan_stops; return 0; }
int bt_le_adv_start(const struct bt_le_adv_param *params, const struct bt_data *ad,
                    size_t ad_count, const struct bt_data *rsp, size_t rsp_count)
{
    (void)ad; (void)ad_count;
    ++adv_starts;
    adv_options = params->options;
    adv_rsp_null = rsp == NULL;
    adv_rsp_count = rsp_count;
    if (fail_adv_starts) { --fail_adv_starts; return -EIO; }
    return 0;
}
int bt_le_adv_stop(void) { ++adv_stops; return 0; }
int bt_le_adv_update_data(const struct bt_data *ad, size_t ad_count,
                          const struct bt_data *rsp, size_t rsp_count)
{
    (void)ad; (void)ad_count;
    ++adv_updates;
    adv_rsp_null = rsp == NULL;
    adv_rsp_count = rsp_count;
    if (fail_adv_updates) { --fail_adv_updates; return -EIO; }
    return 0;
}
bool gw_peripheral_busy(void) { return peripheral_busy; }
void gw_peripheral_set_admission(bool enabled) { peripheral_admission = enabled; }
void bt_conn_foreach(int type, void (*callback)(struct bt_conn *, void *), void *user)
{ static struct bt_conn conn; assert(type == BT_CONN_TYPE_LE); if (stack_connection) { callback(&conn, user); } }
uint8_t gw_gatt_connection_state(void) { return connection_state; }
uint32_t gw_gatt_connection_id(void) { return connection_id; }
void gw_gatt_connection_snapshot(uint32_t *id, uint8_t *state)
{ *id = connection_id; *state = connection_state; }
bool gw_app_record(uint8_t type, const uint8_t *record, size_t length)
{
    assert(type == 0x20 && length <= sizeof(last_record));
    memcpy(last_record, record, length);
    last_record_length = length;
    ++record_count;
    return true;
}

static uint8_t response[GW_PAYLOAD_MAX];
static size_t response_length;

static uint8_t command(uint8_t opcode, const uint8_t *args, size_t length)
{ return gw_radio_command(opcode, args, length, response, &response_length); }

static uint8_t configure_scan(struct gw_profile *profile)
{
    uint8_t request[GW_PROFILE_MAX];
    size_t length = gw_profile_encode(profile, true, request, sizeof(request));
    assert(length != 0);
    return command(5, request, length);
}

static size_t make_advertisement(uint8_t *args, uint64_t revision, bool enabled,
                                  bool scannable, bool response_data)
{
    memset(args, 0, 76);
    gw_put_u64(args, revision);
    args[8] = enabled;
    args[9] = scannable;
    gw_put_u16(args + 10, 400);
    args[12] = 3;
    args[13] = 2; args[14] = 1; args[15] = 6;
    args[16] = response_data ? 3 : 0;
    if (response_data) { args[17] = 2; args[18] = 9; args[19] = 'x'; }
    return response_data ? 20 : 17;
}

int main(void)
{
    struct gw_profile profile, actual;
    uint8_t advertisement[76];
    size_t length;
    assert(gw_radio_init() == 0 && registered_scan != NULL);
    assert((gw_radio_capabilities() & BIT(9)) != 0);
    assert((gw_radio_capabilities() & BIT(0)) == 0);
    gw_profile_default(&profile);
    profile.revision = 1;
    profile.scan_flags = 3;
    assert(configure_scan(&profile) == GW_OK);
    assert(scan_starts == 1 && scan_params.type == BT_LE_SCAN_TYPE_ACTIVE);
    assert(scan_params.interval == 160 && scan_params.window == 80);
    assert(configure_scan(&profile) == GW_OK && scan_starts == 1);
    profile.window_ticks = 100;
    assert(configure_scan(&profile) == GW_CONFLICT);
    profile.revision = 2;
    fail_scan_starts = 1;
    assert(configure_scan(&profile) == GW_APPLY_FAILED);
    assert(scan_starts == 3 && scan_stops == 1 && gw_radio_scan_state() == 1);
    assert(command(6, NULL, 0) == GW_OK);
    assert(gw_profile_parse(response, response_length, true, &actual));
    assert(actual.revision == 1 && actual.window_ticks == 80);
    connection_state = 2;
    assert(configure_scan(&profile) == GW_BUSY);
    connection_state = 0;

    profile.company_count = 1;
    profile.companies[0] = 0x004c;
    profile.min_rssi = -70;
    assert(configure_scan(&profile) == GW_OK);
    uint8_t ad[] = {5,0xff,0x4c,0,0x02,0x15};
    struct net_buf_simple buffer = {.data = ad, .len = sizeof(ad)};
    bt_addr_le_t unbonded = {.type = 1, .a = {.val = {0xaa,0xbb,0xcc,0xdd,0xee,0xff}}};
    struct bt_le_scan_recv_info info = {
        .addr = &unbonded, .adv_props = BT_GAP_ADV_PROP_CONNECTABLE | BT_GAP_ADV_PROP_SCANNABLE,
        .rssi = -60
    };
    registered_scan->recv(&info, &buffer);
    assert(record_count == 1 && last_record_length == 24 + sizeof(ad));
    assert(gw_get_u64(last_record) == (uint64_t)radio_mock_now);
    assert(last_record[10] == 1 && last_record[11] == 0xff && last_record[16] == 0xaa);
    assert(memcmp(last_record + 24, ad, sizeof(ad)) == 0);
    info.addr = &bond.addr;
    registered_scan->recv(&info, &buffer);
    assert(record_count == 2 && last_record[10] == 0xff && last_record[9] == 3);
    assert(memcmp(last_record + 11, "\0\0\0\0\0\0", 6) == 0);
    assert(memcmp(last_record + 24, ad, sizeof(ad)) == 0);
    ad[2] = 0x4d;
    registered_scan->recv(&info, &buffer);
    assert(record_count == 2);
    ad[2] = 0x4c;
    info.rssi = 127;
    registered_scan->recv(&info, &buffer);
    assert(record_count == 2);
    info.rssi = -60;
    info.adv_props |= BT_GAP_ADV_PROP_EXT_ADV;
    registered_scan->recv(&info, &buffer);
    assert(record_count == 2);
    uint64_t seen, filtered, incomplete;
    gw_radio_counters(&seen, &filtered, &incomplete);
    assert(seen == 5 && filtered == 2 && incomplete == 1);

    length = make_advertisement(advertisement, 1, true, false, false);
    assert(command(7, advertisement, length) == GW_BUSY);
    profile.revision = 3;
    profile.scan_flags = 0;
    assert(configure_scan(&profile) == GW_OK);
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_starts == 1 && adv_rsp_null && adv_rsp_count == 0 && adv_options == 0);
    assert(command(7, advertisement, length) == GW_OK && adv_starts == 1);
    advertisement[10]++;
    assert(command(7, advertisement, length) == GW_CONFLICT);
    assert(gw_radio_prepare_connect() == -EBUSY);
    length = make_advertisement(advertisement, 2, true, true, false);
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_options == BT_LE_ADV_OPT_SCANNABLE && adv_rsp_null);
    length = make_advertisement(advertisement, 3, true, true, true);
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_options == BT_LE_ADV_OPT_SCANNABLE && !adv_rsp_null && adv_rsp_count == 1);
    length = make_advertisement(advertisement, 4, true, false, false);
    fail_adv_starts = 1;
    assert(command(7, advertisement, length) == GW_APPLY_FAILED);
    assert(command(8, NULL, 0) == GW_OK && gw_get_u64(response) == 3);
    assert(command(9, NULL, 0) == GW_OK && response[1] == 1);
    length = make_advertisement(advertisement, 4, false, false, false);
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_stops == 3);

    profile.revision = 4;
    profile.scan_flags = 1;
    assert(configure_scan(&profile) == GW_OK);
    assert(gw_radio_prepare_connect() == 0 && gw_radio_scan_state() == 3);
    assert(configure_scan(&profile) == GW_BUSY);
    connection_state = 2;
    connection_id = 77;
    gw_radio_connect_complete();
    gw_radio_poll();
    assert(gw_radio_scan_state() == 3);
    length = make_advertisement(advertisement, 5, true, false, false);
    assert(command(7, advertisement, length) == GW_BUSY);
    assert(command(9, NULL, 0) == GW_OK);
    assert(gw_get_u32(response + 10) == 77 && response[14] == 2);
    /* Disabling scan while connected prevents a later automatic resume. */
    profile.revision = 5;
    profile.scan_flags = 0;
    assert(configure_scan(&profile) == GW_OK && gw_radio_scan_state() == 0);
    connection_state = 0;
    connection_id = 0;
    gw_radio_poll();
    assert(gw_radio_scan_state() == 0);
    profile.revision = 6;
    profile.scan_flags = 1;
    assert(configure_scan(&profile) == GW_OK);
    assert(gw_radio_prepare_connect() == 0 && gw_radio_scan_state() == 3);
    gw_radio_connect_complete();
    gw_radio_poll();
    assert(gw_radio_scan_state() == 1);
    /* Outward advertising management: live data, rollback and peripherals. */
    profile.revision = 7; profile.scan_flags = 0;
    assert(configure_scan(&profile) == GW_OK);
    length = make_advertisement(advertisement, 5, true, true, true);
    assert(command(7, advertisement, length) == GW_OK);
    unsigned int prior_starts = adv_starts, prior_stops = adv_stops;
    gw_put_u64(advertisement, 6);
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_starts == prior_starts && adv_stops == prior_stops);
    gw_put_u64(advertisement, 7); advertisement[19] = 'y';
    unsigned int prior_updates = adv_updates;
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_updates == prior_updates + 1 && adv_stops == prior_stops);
    gw_put_u64(advertisement, 8); advertisement[19] = 'z';
    fail_adv_updates = 1;
    assert(command(7, advertisement, length) == GW_APPLY_FAILED);
    assert(command(8, NULL, 0) == GW_OK && gw_get_u64(response) == 7 && response[19] == 'y');
    assert(command(0x0c, NULL, 0) == GW_OK && response_length == 35 && response[9] == 1);
    assert((int32_t)gw_get_u32(response + 15) == -EIO);
    fail_adv_updates = 2;
    assert(command(7, advertisement, length) == GW_APPLY_FAILED);
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 2);
    assert(gw_radio_prepare_connect() == -EBUSY);
    profile.revision = 8; profile.scan_flags = 1;
    assert(configure_scan(&profile) == GW_BUSY);
    uint8_t enabled[9]; gw_put_u64(enabled, 8); enabled[8] = 0;
    assert(command(0x0b, enabled, 8) == GW_BAD_PARAM);
    assert(command(0x0b, enabled, 9) == GW_OK);
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 0 && gw_get_u32(response + 15) == 0);
    length = make_advertisement(advertisement, 9, true, true, true);
    advertisement[9] = 2;
    assert(command(7, advertisement, length) == GW_OK);
    assert(adv_options == (BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE));
    peripheral_busy = true;
    gw_radio_peripheral_changed(); gw_radio_poll();
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 3);
    assert(gw_radio_enter_maintenance() == -EBUSY);
    gw_put_u64(enabled, 10); enabled[8] = 0;
    assert(command(0x0b, enabled, 9) == GW_OK);
    peripheral_busy = false; gw_radio_peripheral_changed(); gw_radio_poll();
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 0);
    gw_put_u64(enabled, 11); enabled[8] = 1;
    assert(command(0x0b, enabled, 9) == GW_OK);
    prior_starts = adv_starts;
    peripheral_busy = true; gw_radio_peripheral_changed(); gw_radio_poll();
    peripheral_busy = false; gw_radio_peripheral_changed(); gw_radio_poll();
    assert(adv_starts == prior_starts + 1);
    /* Preserve a fast connect/disconnect edge even when current busy is false. */
    prior_starts = adv_starts;
    gw_radio_peripheral_connected(); gw_radio_peripheral_changed(); gw_radio_poll();
    assert(adv_starts == prior_starts + 1);
    stack_connection = true;
    assert(gw_radio_enter_maintenance() == -EBUSY && !peripheral_admission);
    stack_connection = false;
    gw_radio_poll();
    assert(peripheral_admission);
    assert(gw_radio_enter_maintenance() == 0);
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 3);
    gw_put_u64(enabled, 12);
    assert(command(0x0b, enabled, 9) == GW_BUSY);
    gw_radio_leave_maintenance();
    assert(command(0x0c, NULL, 0) == GW_OK && response[9] == 1);
    enabled[8] = 0; assert(command(0x0b, enabled, 9) == GW_OK);
    assert(configure_scan(&profile) == GW_OK);
    assert(gw_radio_enter_maintenance() == 0 && gw_radio_scan_state() == 3);
    gw_radio_leave_maintenance(); assert(gw_radio_scan_state() == 1);
    puts("gw_radio: scan/ADV rollback, role gating, bonded-address redaction tests passed");
    return 0;
}
