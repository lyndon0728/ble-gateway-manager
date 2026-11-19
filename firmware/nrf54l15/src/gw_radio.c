#include "gw_radio.h"
#include "gw_core.h"
#include "gw_filter_v2.h"
#include "gw_gatt.h"
#include "gw_peripheral.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/settings/settings.h>

K_MUTEX_DEFINE(radio_lock);
static struct gw_filter_v2_config filter_config, filter_candidate;
static struct gw_filter_v2_state filter_state;
static struct gw_filter_v2_pending filter_input[32];
static uint8_t filter_input_head, filter_input_count;
_Static_assert(sizeof(filter_input) + sizeof(filter_state.pending) + 2 <= 4096,
               "V2 ingress and PAIR caches exceed the shared memory budget");
static uint8_t filter_old_bytes[GW_FILTER_V2_MAX], filter_new_bytes[GW_FILTER_V2_MAX];
#define profile (filter_config.legacy)
static struct gw_profile candidate;
static uint8_t profile_bytes[GW_PROFILE_MAX];
static uint8_t candidate_bytes[GW_PROFILE_MAX];
static uint8_t scan_state;
static uint8_t adv_state;
static uint8_t adv_config[76];
static size_t adv_length = 14;
static int adv_last_error;
static uint64_t adv_last_apply_ms;
static uint32_t adv_live_updates, adv_start_count;
static atomic_t peripheral_changed;
static atomic_t peripheral_advertising_ended;
static bool maintenance;
static bool maintenance_waiting;
static bool radio_ready;
static atomic_t connect_paused;
static atomic_t resume_requested;
static uint64_t reports_seen, reports_incomplete;
static atomic_t callback_contention;
static bt_addr_le_t bonded_identities[CONFIG_BT_MAX_PAIRED];
static size_t bonded_identity_count;
static bool bonded_identity_overflow;

static void remember_bond(const struct bt_bond_info *bond, void *unused)
{
    ARG_UNUSED(unused);
    if (bonded_identity_count == ARRAY_SIZE(bonded_identities)) {
        bonded_identity_overflow = true;
        return;
    }
    bonded_identities[bonded_identity_count++] = bond->addr;
}

static bool address_is_observed(const bt_addr_le_t *addr)
{
    if (!addr || bonded_identity_overflow ||
        (addr->type != BT_ADDR_LE_PUBLIC && addr->type != BT_ADDR_LE_RANDOM)) {
        return false;
    }
    /* NCS resolves known IRKs before this callback; a bond identity cannot
     * prove the address observed on air. New pairing is disabled, so the
     * settings snapshot remains fixed throughout this boot. */
    for (size_t i = 0; i < bonded_identity_count; ++i) {
        if (bt_addr_le_eq(addr, &bonded_identities[i])) {
            return false;
        }
    }
    return true;
}

static int start_scan(void)
{
    if (!(profile.scan_flags & 1)) {
        scan_state = 0;
        return 0;
    }
    struct bt_le_scan_param params = {
        .type = (profile.scan_flags & 2) ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = profile.interval_ticks,
        .window = profile.window_ticks,
    };
    int err = bt_le_scan_start(&params, NULL);
    scan_state = err ? 2 : 1;
    return err;
}

static void filter_emit(void *user, const uint8_t *record, size_t length)
{
    ARG_UNUSED(user);
    gw_app_record(0x20, record, length);
}

static void filter_discard(void)
{
    gw_filter_v2_clear(&filter_state);
    filter_state.counters[GW_FILTER_SEEN] += filter_input_count;
    filter_state.counters[GW_FILTER_CONFIG_DISCARDED] += filter_input_count;
    filter_input_count = filter_input_head = 0;
}

static void filter_drain(void)
{
    while (filter_input_count) {
        struct gw_filter_v2_pending *input = &filter_input[filter_input_head];
        gw_filter_v2_feed(&filter_state, &filter_config, input->record, input->length,
                          filter_emit, NULL);
        filter_input_head = (filter_input_head + 1) % ARRAY_SIZE(filter_input);
        --filter_input_count;
    }
    /* All published capture timestamps <= this watermark have been consumed.
     * Producers also acquire radio_lock before assigning their timestamp. */
    gw_filter_v2_advance(&filter_state, (uint64_t)k_uptime_get());
}

static void scan_received(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
    uint8_t record[24 + 31];
    if (k_mutex_lock(&radio_lock, K_NO_WAIT) != 0) {
        atomic_inc(&callback_contention);
        return;
    }
    reports_seen++;
    bool extended = (info->adv_props & BT_GAP_ADV_PROP_EXT_ADV) != 0;
    bool response = (info->adv_props & BT_GAP_ADV_PROP_SCAN_RESPONSE) != 0;
    uint8_t kind = (extended ? 2 : 0) + (response ? 1 : 0);
    /* Extended reports remain disabled until reassembly and board RF are qualified. */
    if (extended || ad->len > 31) {
        reports_incomplete++;
        if (filter_config.engine) {
            ++filter_state.counters[GW_FILTER_SEEN];
            ++filter_state.counters[GW_FILTER_UNSUPPORTED_CONTEXT];
        }
        k_mutex_unlock(&radio_lock);
        return;
    }
    int8_t rssi = info->rssi == 127 ? -128 : info->rssi;
    memset(record, 0, 24);
    gw_put_u64(record, k_uptime_get());
    record[8] = kind;
    record[9] = (info->adv_props & BT_GAP_ADV_PROP_SCANNABLE) ? 2 : 0;
    if (!response && (info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) {
        record[9] |= 1;
    }
    if (info->adv_props & BT_GAP_ADV_PROP_DIRECTED) {
        record[9] |= 4;
    }
    record[10] = 0xff;
    if (address_is_observed(info->addr)) {
        record[10] = info->addr->type;
        for (size_t i = 0; i < 6; ++i) {
            record[11 + i] = info->addr->a.val[5 - i];
        }
    }
    record[17] = (uint8_t)rssi;
    record[18] = 0x80;
    record[19] = 1;
    record[21] = 0xff;
    gw_put_u16(record + 22, ad->len);
    memcpy(record + 24, ad->data, ad->len);
    if (scan_state == 1) {
        if (!filter_config.engine) {
            gw_filter_v2_feed(&filter_state, &filter_config, record, 24 + ad->len, filter_emit, NULL);
        } else if (filter_input_count == ARRAY_SIZE(filter_input)) {
            ++filter_state.counters[GW_FILTER_SEEN];
            ++filter_state.counters[GW_FILTER_PAIR_OVERFLOW];
        } else {
            struct gw_filter_v2_pending *input =
                &filter_input[(filter_input_head + filter_input_count) % ARRAY_SIZE(filter_input)];
            input->length = 24 + ad->len;
            memcpy(input->record, record, input->length);
            ++filter_input_count;
        }
    } else {
        ++filter_state.counters[GW_FILTER_SEEN];
        ++filter_state.counters[GW_FILTER_UNSUPPORTED_CONTEXT];
    }
    k_mutex_unlock(&radio_lock);
}

static struct bt_le_scan_cb scan_callbacks = {.recv = scan_received};

static uint8_t apply_scan(const uint8_t *args, size_t length, bool extended)
{
    if (filter_config.engine) return GW_CONFLICT;
    if (!gw_profile_parse(args, length, extended, &candidate)) {
        return GW_BAD_PARAM;
    }
    if (!extended && profile.rule_count) {
        return GW_CONFLICT;
    }
    size_t old_length = gw_profile_encode(&profile, true, profile_bytes, sizeof(profile_bytes));
    size_t new_length = gw_profile_encode(&candidate, true, candidate_bytes, sizeof(candidate_bytes));
    if (candidate.revision < profile.revision) {
        return GW_CONFLICT;
    }
    if (candidate.revision == profile.revision) {
        if (old_length != new_length || memcmp(profile_bytes, candidate_bytes, old_length)) {
            return GW_CONFLICT;
        }
        if (scan_state == 2) {
            return GW_APPLY_FAILED;
        }
        return scan_state == ((profile.scan_flags & 1) ? 1 : 0) ? GW_OK : GW_BUSY;
    }
    if ((candidate.scan_flags & 1) && (maintenance || gw_peripheral_busy() ||
        atomic_get(&connect_paused) || gw_gatt_connection_state() != 0 || adv_state != 0)) {
        return GW_BUSY;
    }
    if (!radio_ready && (candidate.scan_flags & 1)) {
        return GW_UNSUPPORTED;
    }
    if (scan_state == 1 && bt_le_scan_stop() != 0) {
        return GW_APPLY_FAILED;
    }
    filter_discard();
    profile = candidate;
    if (start_scan() == 0) {
        return GW_OK;
    }
    /* The serialized old snapshot is retained until the replacement starts. */
    gw_profile_parse(profile_bytes, old_length, true, &profile);
    if (start_scan() != 0) {
        scan_state = 2;
    }
    return GW_APPLY_FAILED;
}

static uint8_t apply_scan_v2(const uint8_t *args, size_t length)
{
    if (!gw_filter_v2_parse(args, length, &filter_candidate)) return GW_BAD_PARAM;
    size_t old_length = gw_filter_v2_encode(&filter_config, filter_old_bytes, sizeof(filter_old_bytes));
    size_t new_length = gw_filter_v2_encode(&filter_candidate, filter_new_bytes, sizeof(filter_new_bytes));
    if (!old_length || !new_length) return GW_BAD_PARAM;
    if (filter_candidate.legacy.revision < profile.revision) return GW_CONFLICT;
    if (filter_candidate.legacy.revision == profile.revision) {
        if (old_length != new_length || memcmp(filter_old_bytes, filter_new_bytes, old_length)) return GW_CONFLICT;
        if (scan_state == 2) return GW_APPLY_FAILED;
        return scan_state == ((profile.scan_flags & 1) ? 1 : 0) ? GW_OK : GW_BUSY;
    }
    if ((filter_candidate.legacy.scan_flags & 1) && (maintenance || gw_peripheral_busy() ||
        atomic_get(&connect_paused) || gw_gatt_connection_state() != 0 || adv_state != 0)) return GW_BUSY;
    if (!radio_ready) return GW_UNSUPPORTED;
    if (scan_state == 1 && bt_le_scan_stop() != 0) return GW_APPLY_FAILED;
    filter_discard();
    filter_config = filter_candidate;
    if (start_scan() == 0) return GW_OK;
    if (!gw_filter_v2_parse(filter_old_bytes, old_length, &filter_config) || start_scan() != 0) scan_state = 2;
    return GW_APPLY_FAILED;
}

static size_t split_ad(const uint8_t *raw, size_t length, struct bt_data *out)
{
    size_t count = 0;
    for (size_t pos = 0; pos < length && raw[pos]; pos += raw[pos] + 1) {
        out[count].type = raw[pos + 1];
        out[count].data_len = raw[pos] - 1;
        out[count].data = raw + pos + 2;
        count++;
    }
    return count;
}

static int start_advertising(const uint8_t *config)
{
    if (!config[8]) {
        adv_state = 0;
        return 0;
    }
    struct bt_data ad[16], rsp[16];
    size_t ad_size = config[12];
    size_t rsp_size = config[13 + ad_size];
    size_t ad_count = split_ad(config + 13, ad_size, ad);
    size_t rsp_count = split_ad(config + 14 + ad_size, rsp_size, rsp);
    struct bt_le_adv_param params = {
        .id = BT_ID_DEFAULT,
        .options = config[9] == 2 ? (BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE) :
                   config[9] == 1 ? BT_LE_ADV_OPT_SCANNABLE : BT_LE_ADV_OPT_NONE,
        .interval_min = gw_get_u16(config + 10),
        .interval_max = gw_get_u16(config + 10),
    };
    /* The legacy host treats a non-NULL sd pointer as scannable even at len 0. */
    int err = bt_le_adv_start(&params, ad, ad_count, rsp_count ? rsp : NULL, rsp_count);
    adv_state = err ? 2 : 1;
    if (!err) {
        adv_start_count++;
    }
    return err;
}

static int update_advertising_data(const uint8_t *config)
{
    struct bt_data ad[16], rsp[16];
    size_t ad_size = config[12], rsp_size = config[13 + ad_size];
    size_t ad_count = split_ad(config + 13, ad_size, ad);
    size_t rsp_count = split_ad(config + 14 + ad_size, rsp_size, rsp);
    return bt_le_adv_update_data(ad, ad_count, rsp_count ? rsp : NULL, rsp_count);
}

static void sync_peripheral_advertising(void)
{
    /* A successful peripheral connection automatically terminates this
     * connectable advertiser in the controller. The desired config persists. */
    bool ended = atomic_set(&peripheral_advertising_ended, 0) != 0;
    if ((ended || gw_peripheral_busy()) && adv_config[9] == 2 && adv_state == 1) {
        adv_state = adv_config[8] ? 3 : 0;
    }
}

static uint8_t apply_advertising(const uint8_t *args, size_t length)
{
    if (length < 14 || args[8] > 1 || args[9] > 2 || args[12] > 31 ||
        length < 14U + args[12]) {
        return GW_BAD_PARAM;
    }
    sync_peripheral_advertising();
    size_t rsp_size = args[13 + args[12]];
    if (rsp_size > 31 || length != 14U + args[12] + rsp_size ||
        (!args[9] && rsp_size) || gw_get_u16(args + 10) < 160 ||
        gw_get_u16(args + 10) > 16384 ||
        !gw_ad_valid(args + 13, args[12]) ||
        !gw_ad_valid(args + 14 + args[12], rsp_size)) {
        return GW_BAD_PARAM;
    }
    uint64_t revision = gw_get_u64(args);
    if (revision < gw_get_u64(adv_config)) {
        return GW_CONFLICT;
    }
    if (revision == gw_get_u64(adv_config)) {
        if (length != adv_length || memcmp(args, adv_config, length)) {
            return GW_CONFLICT;
        }
        if (adv_state == 2) {
            return GW_APPLY_FAILED;
        }
        return adv_state == (adv_config[8] ? 1 : 0) ||
               (adv_config[8] && adv_config[9] == 2 && adv_state == 3 && gw_peripheral_busy())
               ? GW_OK : GW_BUSY;
    }
    if (args[8] && !radio_ready) {
        return GW_UNSUPPORTED;
    }
    if (args[8] && (maintenance || scan_state != 0 || gw_peripheral_busy() ||
                   atomic_get(&connect_paused) || gw_gatt_connection_state() != 0)) {
        return GW_BUSY;
    }
    bool unchanged = length == adv_length && !memcmp(args + 8, adv_config + 8, length - 8);
    if (unchanged && adv_state == (args[8] ? 1 : 0)) {
        /* Revision-only changes have no radio side effects. */
        goto commit;
    }
    if (adv_state == 1 && args[8] && args[9] == adv_config[9] &&
        gw_get_u16(args + 10) == gw_get_u16(adv_config + 10)) {
        int err = update_advertising_data(args);
        if (!err) {
            adv_live_updates++;
            goto commit;
        }
        adv_last_error = err;
        /* AD and ScanRsp are separate HCI writes. Restore BOTH on any error;
         * this is recovery, not an atomic over-air payload transaction. */
        if (update_advertising_data(adv_config) != 0) {
            (void)bt_le_adv_stop();
            adv_state = 2;
        }
        return GW_APPLY_FAILED;
    }
    if (adv_state == 1 || adv_state == 2) {
        int err = bt_le_adv_stop();
        if (err) {
            adv_last_error = err;
            adv_state = 2;
            return GW_APPLY_FAILED;
        }
        adv_state = 0;
    }
    int err = start_advertising(args);
    if (err) {
        adv_last_error = err;
        if (start_advertising(adv_config) != 0) {
            adv_state = 2;
        }
        return GW_APPLY_FAILED;
    }
commit:
    memcpy(adv_config, args, length);
    adv_length = length;
    adv_last_error = 0;
    adv_last_apply_ms = k_uptime_get();
    return GW_OK;
}

int gw_radio_init(void)
{
    gw_filter_v2_default(&filter_config);
    memset(&filter_state, 0, sizeof(filter_state));
    filter_input_head = filter_input_count = 0;
    gw_put_u16(adv_config + 10, 160);
    if (!IS_ENABLED(CONFIG_GW_RADIO_VERIFIED)) {
        return -ENOTSUP;
    }
    int err = bt_enable(NULL);
    if (err) {
        return err;
    }
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        err = settings_load();
        if (err) {
            return err;
        }
    }
    bt_foreach_bond(BT_ID_DEFAULT, remember_bond, NULL);
    radio_ready = true;
    bt_le_scan_cb_register(&scan_callbacks);
    return 0;
}

uint32_t gw_radio_capabilities(void)
{
    /* Role combinations are gated until tested with the selected FEM and PCB. */
    return radio_ready ? (BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(9) | BIT(10) | BIT(11) | BIT(13)) : 0;
}

uint8_t gw_radio_command(uint8_t opcode, const uint8_t *args, size_t length,
                         uint8_t *result, size_t *result_length)
{
    uint8_t status = GW_OK;
    *result_length = 0;
    k_mutex_lock(&radio_lock, K_FOREVER);
    sync_peripheral_advertising();
    switch (opcode) {
    case 1:
    case 5:
        status = apply_scan(args, length, opcode == 5);
        if (status == GW_OK) {
            *result_length = gw_profile_encode(&profile, opcode == 5, result, GW_PAYLOAD_MAX - 2);
        }
        break;
    case 2:
    case 6:
        if (opcode == 6 && filter_config.engine) {
            status = GW_CONFLICT;
        } else if (length) {
            status = GW_BAD_PARAM;
        } else {
            *result_length = gw_profile_encode(&profile, opcode == 6, result, GW_PAYLOAD_MAX - 2);
        }
        break;
    case 0x19:
        status = apply_scan_v2(args, length);
        if (status != GW_OK) break;
        *result_length = gw_filter_v2_encode(&filter_config, result, GW_PAYLOAD_MAX - 2);
        break;
    case 0x1a:
    case 0x1b:
        if (length) status = GW_BAD_PARAM;
        else if (!radio_ready) status = GW_UNSUPPORTED;
        else if (opcode == 0x1a) *result_length = gw_filter_v2_encode(&filter_config, result, GW_PAYLOAD_MAX - 2);
        else { filter_drain(); gw_filter_v2_status(&filter_state, &filter_config, result); *result_length = 96; }
        break;
    case 7:
        status = apply_advertising(args, length);
        if (status != GW_OK) {
            break;
        }
        memcpy(result, adv_config, adv_length);
        *result_length = adv_length;
        break;
    case 0x0b: {
        if (length != 9 || args[8] > 1) {
            status = GW_BAD_PARAM;
            break;
        }
        uint8_t replacement[sizeof(adv_config)];
        memcpy(replacement, adv_config, adv_length);
        memcpy(replacement, args, 9);
        status = apply_advertising(replacement, adv_length);
        if (status == GW_OK) {
            memcpy(result, adv_config, adv_length);
            *result_length = adv_length;
        }
        break;
    }
    case 0x0c:
        if (length) {
            status = GW_BAD_PARAM;
            break;
        }
        memcpy(result, adv_config, 9);
        result[9] = adv_state;
        result[10] = adv_config[9];
        memcpy(result + 11, adv_config + 10, 3);
        result[14] = adv_config[13 + adv_config[12]];
        gw_put_u32(result + 15, (uint32_t)adv_last_error);
        gw_put_u64(result + 19, adv_last_apply_ms);
        gw_put_u32(result + 27, adv_live_updates);
        gw_put_u32(result + 31, adv_start_count);
        *result_length = 35;
        break;
    case 8:
        if (length) {
            status = GW_BAD_PARAM;
        } else {
            memcpy(result, adv_config, adv_length);
            *result_length = adv_length;
        }
        break;
    case 9:
        if (length) {
            status = GW_BAD_PARAM;
        } else {
            result[0] = scan_state;
            result[1] = adv_state;
            gw_put_u64(result + 2, gw_get_u64(adv_config));
            uint32_t connection;
            gw_gatt_connection_snapshot(&connection, result + 14);
            gw_put_u32(result + 10, connection);
            *result_length = 15;
        }
        break;
    default:
        status = GW_UNSUPPORTED;
        break;
    }
    k_mutex_unlock(&radio_lock);
    return status;
}

int gw_radio_prepare_connect(void)
{
    k_mutex_lock(&radio_lock, K_FOREVER);
    if (!radio_ready || maintenance || gw_peripheral_busy() || adv_state != 0) {
        k_mutex_unlock(&radio_lock);
        return -EBUSY;
    }
    int err = 0;
    if (scan_state == 1) {
        err = bt_le_scan_stop();
    }
    if (!err) {
        filter_discard();
        atomic_set(&connect_paused, 1);
        scan_state = (profile.scan_flags & 1) ? 3 : 0;
    }
    k_mutex_unlock(&radio_lock);
    return err;
}

void gw_radio_connect_complete(void)
{
    atomic_set(&resume_requested, 1);
}

void gw_radio_peripheral_changed(void)
{
    atomic_set(&peripheral_changed, 1);
}

void gw_radio_peripheral_connected(void)
{
    atomic_set(&peripheral_advertising_ended, 1);
    atomic_set(&peripheral_changed, 1);
}

static void count_connection(struct bt_conn *conn, void *count)
{
    ARG_UNUSED(conn);
    (*(unsigned int *)count)++;
}

static bool stack_connections_alive(void)
{
    unsigned int count = 0;
    /* Includes allocated objects whose normal RX callback is still queued.
     * HCI command completion alone is not a normal-RX drain barrier. */
    bt_conn_foreach(BT_CONN_TYPE_LE, count_connection, &count);
    return count != 0;
}

void gw_radio_poll(void)
{
    k_mutex_lock(&radio_lock, K_FOREVER);
    filter_drain();
    k_mutex_unlock(&radio_lock);
    if (maintenance_waiting && !stack_connections_alive() &&
        !gw_peripheral_busy() && gw_gatt_connection_state() == 0) {
        /* BEGIN returned BUSY, so no DFU session owns the reservation. Restore
         * desired roles after the late connection has completely recycled. */
        gw_radio_leave_maintenance();
    }
    if (atomic_get(&peripheral_changed)) {
        k_mutex_lock(&radio_lock, K_FOREVER);
        atomic_clear(&peripheral_changed);
        sync_peripheral_advertising();
        if (!gw_peripheral_busy() && adv_state == 3 && adv_config[8] && !maintenance) {
            int err = start_advertising(adv_config);
            if (err) { adv_last_error = err; }
        }
        k_mutex_unlock(&radio_lock);
    }
    /* The baseline permits scan OR a connection. Resume only after disconnect. */
    if (atomic_get(&resume_requested) && gw_gatt_connection_state() == 0 &&
        !maintenance && !gw_peripheral_busy()) {
        k_mutex_lock(&radio_lock, K_FOREVER);
        atomic_clear(&resume_requested);
        atomic_clear(&connect_paused);
        start_scan();
        k_mutex_unlock(&radio_lock);
    }
}

int gw_radio_enter_maintenance(void)
{
    k_mutex_lock(&radio_lock, K_FOREVER);
    if (gw_peripheral_busy() || gw_gatt_connection_state() != 0 ||
        atomic_get(&connect_paused)) {
        k_mutex_unlock(&radio_lock);
        return -EBUSY;
    }
    if (maintenance) {
        if (maintenance_waiting && stack_connections_alive()) {
            k_mutex_unlock(&radio_lock);
            return -EBUSY;
        }
        maintenance_waiting = false;
        k_mutex_unlock(&radio_lock);
        return 0;
    }
    gw_peripheral_set_admission(false);
    if ((adv_state == 1 || adv_state == 2) && bt_le_adv_stop() != 0) {
        adv_state = 2;
        gw_peripheral_set_admission(true);
        k_mutex_unlock(&radio_lock);
        return -EIO;
    }
    adv_state = 0;
    if ((scan_state == 1 || scan_state == 2) && bt_le_scan_stop() != 0) {
        scan_state = 2;
        gw_peripheral_set_admission(true);
        k_mutex_unlock(&radio_lock);
        return -EIO;
    }
    filter_discard();
    scan_state = (profile.scan_flags & 1) ? 3 : 0;
    adv_state = adv_config[8] ? 3 : 0;
    maintenance = true;
    maintenance_waiting = stack_connections_alive() || gw_peripheral_busy();
    k_mutex_unlock(&radio_lock);
    return maintenance_waiting ? -EBUSY : 0;
}

void gw_radio_leave_maintenance(void)
{
    k_mutex_lock(&radio_lock, K_FOREVER);
    if (maintenance) {
        maintenance = false;
        maintenance_waiting = false;
        gw_peripheral_set_admission(true);
        atomic_clear(&peripheral_advertising_ended);
        if (adv_config[8]) {
            int err = start_advertising(adv_config);
            if (err) { adv_last_error = err; }
        } else {
            start_scan();
        }
    }
    k_mutex_unlock(&radio_lock);
}

int gw_radio_prepare_dfu(void) { return gw_radio_enter_maintenance(); }
void gw_radio_finish_dfu(void) { gw_radio_leave_maintenance(); }

uint64_t gw_radio_revision(void) { return profile.revision; }
uint8_t gw_radio_scan_state(void) { return scan_state; }
void gw_radio_counters(uint64_t *seen, uint64_t *filtered, uint64_t *incomplete)
{
    k_mutex_lock(&radio_lock, K_FOREVER);
    *seen = reports_seen + (uint32_t)atomic_get(&callback_contention);
    *filtered = 0;
    for (unsigned i = GW_FILTER_RULE_DROPPED; i < GW_FILTER_COUNTER_COUNT; ++i) *filtered += filter_state.counters[i];
    *incomplete = reports_incomplete + (uint32_t)atomic_get(&callback_contention);
    k_mutex_unlock(&radio_lock);
}
