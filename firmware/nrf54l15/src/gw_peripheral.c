#include "gw_peripheral.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

enum { P_OK, P_BAD_PARAM, P_UNSUPPORTED, P_BUSY, P_CONFLICT,
       P_APPLY_FAILED, P_STALE, P_NOT_FOUND };
enum { P_NONE, P_CONNECTED, P_CLOSING };
#define P_VALUE_MAX 512U
#define P_RECORD_HEADER 24U
#define P_RX_CHANNEL 1U
#define P_TX_VALUE_INDEX 6U
#define P_TX_DRAIN_MS 10000

static struct {
    bool initialized, admission, draining, close_sent, close_launching;
    struct bt_conn *conn;
    uint32_t conn_id, next_conn_id;
    uint8_t state, addr_type, addr[6];
    int64_t close_retry_at;
    struct {
        bool pending, launching;
        struct bt_conn *conn; /* Borrowed until callback or recycle barrier. */
        uint64_t seq;
        uint32_t conn_id;
        uint16_t len;
        uint8_t value[P_VALUE_MAX];
        int64_t deadline;
        struct bt_gatt_notify_params params;
    } tx;
    struct {
        uint64_t seq;
        uint32_t conn_id;
        uint16_t len;
        uint8_t value[P_VALUE_MAX];
    } accepted;
} peripheral;
K_MUTEX_DEFINE(peripheral_lock);

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x7be00001, 0x4b5a, 0x4c31, 0x9d91, 0x6c2f9a0b5400));
static struct bt_uuid_128 info_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x7be00002, 0x4b5a, 0x4c31, 0x9d91, 0x6c2f9a0b5400));
static struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x7be00003, 0x4b5a, 0x4c31, 0x9d91, 0x6c2f9a0b5400));
static struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x7be00004, 0x4b5a, 0x4c31, 0x9d91, 0x6c2f9a0b5400));

static ssize_t info_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
{
    uint8_t info[13];
    info[0] = GW_PERIPHERAL_INFO_VERSION;
    sys_put_le32(GW_PERIPHERAL_CAP_RX_WRITE | GW_PERIPHERAL_CAP_TX_NOTIFY |
                 GW_PERIPHERAL_CAP_TX_DEDUP, info + 1);
    sys_put_le64((uint64_t)k_uptime_get(), info + 5);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

static ssize_t rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(attr);
    if (flags & (BT_GATT_WRITE_FLAG_CMD | BT_GATT_WRITE_FLAG_PREPARE |
                 BT_GATT_WRITE_FLAG_EXECUTE)) { return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED); }
    if (offset) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    if (!conn || !buf || !len || len > P_VALUE_MAX || len > bt_gatt_get_mtu(conn) - 3U) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint8_t record[P_RECORD_HEADER + P_VALUE_MAX];
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (conn != peripheral.conn || peripheral.state != P_CONNECTED || !peripheral.admission) {
        k_mutex_unlock(&peripheral_lock);
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    sys_put_le64((uint64_t)k_uptime_get(), record);
    sys_put_le32(peripheral.conn_id, record + 8);
    record[12] = peripheral.addr_type;
    memcpy(record + 13, peripheral.addr, 6);
    sys_put_le16(P_RX_CHANNEL, record + 19);
    record[21] = 0;
    sys_put_le16(len, record + 22);
    memcpy(record + P_RECORD_HEADER, buf, len);
    k_mutex_unlock(&peripheral_lock);
    /* This is opaque application data, never an nRF/ESP control command.
     * ESP owns authentication and any interpretation of these bytes. */
    return gw_app_record(0x22, record, P_RECORD_HEADER + len) ? len :
           BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr); ARG_UNUSED(value);
    /* Always query the stack per connection; restored bonded CCC state and
     * disconnect cleanup must not leave a stale application boolean. */
}

BT_GATT_SERVICE_DEFINE(peripheral_service,
    BT_GATT_PRIMARY_SERVICE(&service_uuid.uuid),
    BT_GATT_CHARACTERISTIC(&info_uuid.uuid, BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ, info_read, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, rx_write, NULL),
    BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE)
);

static bool subscribed(struct bt_conn *conn)
{
    return bt_gatt_is_subscribed(conn, &peripheral_service.attrs[P_TX_VALUE_INDEX],
                                BT_GATT_CCC_NOTIFY);
}

static void notify_complete(struct bt_conn *conn, void *user_data)
{
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (user_data == &peripheral.tx && peripheral.tx.conn == conn) {
        peripheral.tx.pending = false;
    }
    k_mutex_unlock(&peripheral_lock);
    /* Completion releases storage. It is NOT evidence of phone application
     * receipt, and no retry or server-side delivery success is synthesized. */
}

static void peripheral_connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.role != BT_CONN_ROLE_PERIPHERAL) { return; }
    if (err) { gw_radio_peripheral_changed(); return; }
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (peripheral.conn || peripheral.draining) {
        k_mutex_unlock(&peripheral_lock);
        gw_radio_peripheral_connected();
        (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return;
    }
    peripheral.conn = bt_conn_ref(conn);
    peripheral.state = peripheral.admission ? P_CONNECTED : P_CLOSING;
    peripheral.close_sent = false;
    peripheral.close_retry_at = 0;
    if (peripheral.next_conn_id == UINT32_MAX) {
        /* Never wrap a boot-scoped identifier. Refuse further links. */
        peripheral.conn_id = 0;
        peripheral.state = P_CLOSING;
    } else { peripheral.conn_id = ++peripheral.next_conn_id; }
    if (info.le.remote) {
        peripheral.addr_type = info.le.remote->type & 1U;
        for (size_t i = 0; i < 6; ++i) { peripheral.addr[i] = info.le.remote->a.val[5U - i]; }
    } else { peripheral.state = P_CLOSING; }
    k_mutex_unlock(&peripheral_lock);
    gw_radio_peripheral_connected();
}

static void peripheral_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(reason);
    bool changed = false;
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (conn == peripheral.conn) {
        peripheral.conn = NULL;
        peripheral.conn_id = 0;
        peripheral.state = P_NONE;
        peripheral.draining = true;
        peripheral.close_sent = false;
        changed = true;
    }
    k_mutex_unlock(&peripheral_lock);
    if (changed) {
        bt_conn_unref(conn);
        gw_radio_peripheral_changed();
    }
}

static void peripheral_recycled(void)
{
    bool changed = false;
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    /* MAX_CONN=1. ncs-v3.2.1 conn.c flushes TX work before recycle; this
     * barrier also releases a canceled notification whose callback was lost. */
    if (peripheral.draining && !peripheral.conn) {
        peripheral.draining = false;
        peripheral.tx.pending = false;
        peripheral.tx.conn = NULL;
        changed = true;
    }
    k_mutex_unlock(&peripheral_lock);
    if (changed) { gw_radio_peripheral_changed(); }
}

static struct bt_conn_cb peripheral_callbacks = {
    .connected = peripheral_connected,
    .disconnected = peripheral_disconnected,
    .recycled = peripheral_recycled,
};

int gw_peripheral_init(void)
{
    if (peripheral.initialized) { return -EALREADY; }
    peripheral.admission = true;
    bt_conn_cb_register(&peripheral_callbacks);
    peripheral.initialized = true;
    return 0;
}

bool gw_peripheral_busy(void)
{
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    bool busy = peripheral.conn || peripheral.draining || peripheral.tx.launching ||
                peripheral.close_launching;
    k_mutex_unlock(&peripheral_lock);
    return busy;
}

void gw_peripheral_set_admission(bool allowed)
{
    bool changed = false;
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    peripheral.admission = allowed;
    if (!allowed && peripheral.conn && peripheral.state == P_CONNECTED) {
        peripheral.state = P_CLOSING;
        peripheral.close_retry_at = 0;
        changed = true;
    }
    k_mutex_unlock(&peripheral_lock);
    if (changed) { gw_radio_peripheral_changed(); }
}

uint8_t gw_peripheral_command(uint8_t opcode, const uint8_t *args, size_t len,
                             uint8_t *out, size_t *out_len)
{
    if (!out || !out_len || (!args && len)) { return P_BAD_PARAM; }
    *out_len = 0;
    uint8_t status = P_OK;
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (!peripheral.initialized) { status = P_APPLY_FAILED; goto done; }
    if (opcode == 0x0D) {
        if (len) { status = P_BAD_PARAM; goto done; }
        memset(out, 0, 16);
        if (peripheral.conn) {
            sys_put_le32(peripheral.conn_id, out); out[4] = peripheral.state;
            out[5] = peripheral.addr_type; memcpy(out + 6, peripheral.addr, 6);
            sys_put_le16(bt_gatt_get_mtu(peripheral.conn), out + 12);
            out[14] = subscribed(peripheral.conn) ? 1 : 0;
            bt_security_t sec = bt_conn_get_security(peripheral.conn);
            out[15] = sec >= BT_SECURITY_L3 ? 2 : sec >= BT_SECURITY_L2 ? 1 : 0;
        }
        *out_len = 16; goto done;
    }
    if (opcode == 0x0F) {
        if (len != 4) { status = P_BAD_PARAM; goto done; }
        if (!peripheral.conn || !sys_get_le32(args) || sys_get_le32(args) != peripheral.conn_id) {
            status = P_NOT_FOUND; goto done;
        }
        peripheral.state = P_CLOSING;
        goto done;
    }
    if (opcode != 0x0E) { status = P_UNSUPPORTED; goto done; }
    if (len < 15 || len > 14U + P_VALUE_MAX || sys_get_le16(args + 12) != len - 14U) {
        status = P_BAD_PARAM; goto done;
    }
    uint64_t seq = sys_get_le64(args);
    uint32_t id = sys_get_le32(args + 8);
    uint16_t value_len = sys_get_le16(args + 12);
    if (!seq || !id) { status = P_BAD_PARAM; goto done; }
    if (seq == peripheral.accepted.seq) {
        if (id != peripheral.accepted.conn_id || value_len != peripheral.accepted.len ||
            memcmp(args + 14, peripheral.accepted.value, value_len)) {
            status = P_CONFLICT; goto done;
        }
        sys_put_le64(seq, out); *out_len = 8; goto done;
    }
    if (seq < peripheral.accepted.seq) { status = P_STALE; goto done; }
    if (!peripheral.admission || peripheral.tx.pending || peripheral.tx.launching || peripheral.draining ||
        peripheral.state == P_CLOSING) { status = P_BUSY; goto done; }
    if (!peripheral.conn || peripheral.state != P_CONNECTED || id != peripheral.conn_id) {
        status = P_NOT_FOUND; goto done;
    }
    if (value_len > bt_gatt_get_mtu(peripheral.conn) - 3U) { status = P_BAD_PARAM; goto done; }
    if (!subscribed(peripheral.conn)) { status = P_CONFLICT; goto done; }
    peripheral.tx.seq = seq; peripheral.tx.conn_id = id; peripheral.tx.len = value_len;
    memcpy(peripheral.tx.value, args + 14, value_len);
    peripheral.tx.pending = true; peripheral.tx.launching = true;
    peripheral.tx.conn = peripheral.conn;
    peripheral.tx.deadline = k_uptime_get() + P_TX_DRAIN_MS;
    peripheral.tx.params = (struct bt_gatt_notify_params){
        .attr = &peripheral_service.attrs[P_TX_VALUE_INDEX],
        .data = peripheral.tx.value, .len = value_len,
        .func = notify_complete, .user_data = &peripheral.tx,
    };
    struct bt_conn *conn = bt_conn_ref(peripheral.conn);
    k_mutex_unlock(&peripheral_lock);
    int err = bt_gatt_notify_cb(conn, &peripheral.tx.params);
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (!err) {
        peripheral.accepted.seq = seq; peripheral.accepted.conn_id = id;
        peripheral.accepted.len = value_len;
        memcpy(peripheral.accepted.value, peripheral.tx.value, value_len);
        sys_put_le64(seq, out); *out_len = 8;
    } else {
        peripheral.tx.pending = false;
        status = err == -ENOMEM || err == -EAGAIN ? P_BUSY : P_APPLY_FAILED;
    }
    peripheral.tx.launching = false;
    k_mutex_unlock(&peripheral_lock);
    bt_conn_unref(conn);
    return status;
done:
    k_mutex_unlock(&peripheral_lock);
    return status;
}

void gw_peripheral_poll(void)
{
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    int64_t now = k_uptime_get();
    if (peripheral.tx.pending && !peripheral.tx.launching && peripheral.conn &&
        now >= peripheral.tx.deadline) { peripheral.state = P_CLOSING; }
    if (!peripheral.conn || peripheral.state != P_CLOSING || peripheral.close_sent ||
        peripheral.close_launching || peripheral.tx.launching || now < peripheral.close_retry_at) {
        k_mutex_unlock(&peripheral_lock); return;
    }
    struct bt_conn *conn = bt_conn_ref(peripheral.conn);
    peripheral.close_launching = true;
    k_mutex_unlock(&peripheral_lock);
    int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    k_mutex_lock(&peripheral_lock, K_FOREVER);
    if (peripheral.conn == conn) {
        peripheral.close_sent = !err;
        peripheral.close_retry_at = now + 100;
    }
    peripheral.close_launching = false;
    k_mutex_unlock(&peripheral_lock);
    bt_conn_unref(conn);
}
