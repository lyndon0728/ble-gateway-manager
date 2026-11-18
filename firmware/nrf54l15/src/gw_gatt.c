#include "gw_gatt.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

/* One boot-lifetime result is deliberately retained across UART sessions. */
enum { OK, BAD_PARAM, UNSUPPORTED, BUSY, CONFLICT, APPLY_FAILED, STALE, NOT_FOUND };
enum { RUNNING, SUCCEEDED, FAILED, TIMEOUT, UNKNOWN };
enum { E_NONE, E_LINK, E_PEER, E_NOT_DISCOVERED, E_AMBIGUOUS, E_SECURITY,
       E_LENGTH, E_DEADLINE, E_STACK };
enum { NONE, INITIATING, CONNECTED, CLOSING };
enum phase { IDLE, CONNECT_START, SECURITY_START, SECURITY_WAIT, MTU_START,
             MTU_WAIT, DISCONNECT_START, DISCONNECT_WAIT, SERVICE_START,
             SERVICE_WAIT, CHAR_START, CHAR_WAIT, CCC_START, CCC_WAIT,
             READ_START, READ_WAIT, WRITE_START, WRITE_WAIT, SUB_START,
             SUB_WAIT, UNSUB_START, UNSUB_WAIT };

#define VALUE_MAX 512U
#define REQUEST_MAX (10U + 8U + VALUE_MAX)
#define CACHE_COUNT 16U
#define SUB_COUNT 4U

struct handle_entry { uint16_t value, ccc; uint8_t properties; };
struct subscription {
    struct bt_gatt_subscribe_params params;
    bool used, active, removed;
    uint8_t kind, addr_type, addr[6];
    uint32_t conn_id;
};
static struct {
    struct bt_conn *conn;
    uint32_t conn_id, next_conn_id;
    uint8_t conn_state, addr_type, addr[6];
    bool initialized, draining, launching, close_needed, radio_complete;
    void (*record_cb)(const uint8_t *, size_t);
    struct {
        uint64_t seq;
        uint8_t opcode, state, error, att_error;
        uint8_t request[REQUEST_MAX], result[VALUE_MAX + 2U];
        size_t request_len, result_len;
        int64_t deadline;
        enum phase phase;
        bool sent;
        int slot;
    } op;
    struct bt_gatt_discover_params discover;
    struct bt_gatt_read_params read;
    struct bt_gatt_write_params write;
    struct bt_gatt_exchange_params mtu;
    struct bt_uuid_128 service_uuid, char_uuid;
    uint16_t service_start, service_end, declaration, value, ccc, descriptor_end;
    unsigned int matches;
    uint8_t properties;
    struct handle_entry handles[CACHE_COUNT];
    struct subscription subscriptions[SUB_COUNT];
} g;
K_MUTEX_DEFINE(gatt_lock);

static bool running(void) { return g.op.seq && g.op.state == RUNNING; }

static void finish(uint8_t state, uint8_t error, uint8_t att_error)
{
    if (!running()) { return; }
    /* Enforce the deadline in callbacks too, not just at the next poll. */
    if (state != TIMEOUT && state != UNKNOWN && k_uptime_get() >= g.op.deadline) {
        state = g.op.sent && g.op.opcode == 0x14 ? UNKNOWN : TIMEOUT;
        error = E_DEADLINE;
        att_error = 0;
        if (g.conn) { g.close_needed = true; }
    }
    g.op.state = state;
    g.op.error = error;
    g.op.att_error = att_error;
    g.op.phase = IDLE;
    if (g.op.opcode == 0x10 && !g.conn) {
        g.conn_id = 0; g.conn_state = NONE; g.radio_complete = true;
    }
    if (state != SUCCEEDED) { g.op.result_len = 0; }
}

static void fail_stack(int err)
{
    finish(FAILED, err == -EMSGSIZE ? E_LENGTH : E_STACK, 0);
}

static void finish_att(uint8_t err, bool physical_write)
{
    /* Zephyr maps negative host errors (including a lost link) to 0x0e.
     * That value cannot prove a peer ATT rejection. Conservatively retain
     * UNKNOWN for a submitted physical write and never expose a fabricated
     * ATT error code for the indistinguishable host-error case. */
    if (err == BT_ATT_ERR_UNLIKELY) {
        finish(physical_write ? UNKNOWN : FAILED, E_STACK, 0);
        if (physical_write) { g.close_needed = true; }
    } else { finish(err ? FAILED : SUCCEEDED, err ? E_PEER : E_NONE, err); }
}

static void uuid_from_wire(struct bt_uuid_128 *uuid, const uint8_t *wire)
{
    uuid->uuid.type = BT_UUID_TYPE_128;
    for (size_t i = 0; i < 16; ++i) { uuid->val[i] = wire[15U - i]; }
}

static void wire_addr(const bt_addr_le_t *addr, uint8_t *type, uint8_t out[6])
{
    *type = addr->type & 1U;
    for (size_t i = 0; i < 6; ++i) { out[i] = addr->a.val[5U - i]; }
}

static struct handle_entry *handle_find(uint16_t value)
{
    for (size_t i = 0; i < CACHE_COUNT; ++i) {
        if (g.handles[i].value == value) { return &g.handles[i]; }
    }
    return NULL;
}

static void discovery_complete(void)
{
    struct handle_entry *entry = handle_find(g.value);
    if (!entry) { entry = handle_find(0); }
    if (!entry) { finish(FAILED, E_LENGTH, 0); return; }
    *entry = (struct handle_entry){ g.value, g.ccc, g.properties };
    sys_put_le16(g.value, g.op.result);
    sys_put_le16(g.ccc, g.op.result + 2);
    g.op.result[4] = g.properties;
    g.op.result_len = 5;
    finish(SUCCEEDED, E_NONE, 0);
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn != g.conn || params != &g.discover || !running()) {
        k_mutex_unlock(&gatt_lock); return BT_GATT_ITER_STOP;
    }
    enum phase phase = g.op.phase;
    if (!attr) {
        if (phase == SERVICE_WAIT) {
            if (g.matches != 1) {
                finish(FAILED, g.matches ? E_AMBIGUOUS : E_NOT_DISCOVERED, 0);
            } else if (g.op.opcode == 0x18) {
                sys_put_le16(g.service_start, g.op.result);
                sys_put_le16(g.service_end, g.op.result + 2);
                g.op.result_len = 4;
                finish(SUCCEEDED, E_NONE, 0);
            } else { g.op.phase = CHAR_START; }
        } else if (phase == CHAR_WAIT) {
            if (g.matches != 1) {
                finish(FAILED, g.matches ? E_AMBIGUOUS : E_NOT_DISCOVERED, 0);
            } else if (g.value < g.descriptor_end) { g.op.phase = CCC_START; }
            else { discovery_complete(); }
        } else if (phase == CCC_WAIT) {
            if (g.matches > 1) { finish(FAILED, E_AMBIGUOUS, 0); }
            else { discovery_complete(); }
        }
        k_mutex_unlock(&gatt_lock); return BT_GATT_ITER_STOP;
    }
    if (phase == SERVICE_WAIT) {
        const struct bt_gatt_service_val *service = attr->user_data;
        ++g.matches;
        g.service_start = attr->handle;
        g.service_end = service->end_handle;
    } else if (phase == CHAR_WAIT) {
        const struct bt_gatt_chrc *ch = attr->user_data;
        /* Descriptors end before the NEXT characteristic declaration, not at
         * the service end. Otherwise another characteristic's CCC may match. */
        if (g.matches && attr->handle > g.declaration &&
            attr->handle <= g.descriptor_end) {
            g.descriptor_end = attr->handle - 1U;
        }
        if (!bt_uuid_cmp(ch->uuid, &g.char_uuid.uuid)) {
            ++g.matches;
            if (g.matches == 1) {
                g.declaration = attr->handle;
                g.value = ch->value_handle;
                g.properties = ch->properties;
            }
        }
    } else if (phase == CCC_WAIT) { ++g.matches; g.ccc = attr->handle; }
    k_mutex_unlock(&gatt_lock);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_cb(struct bt_conn *conn, uint8_t err,
                      struct bt_gatt_read_params *params,
                      const void *data, uint16_t length)
{
    uint8_t next = BT_GATT_ITER_STOP;
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn == g.conn && params == &g.read && running() &&
        g.op.phase == READ_WAIT) {
        if (err) { finish_att(err, false); }
        else if (!data) {
            sys_put_le16((uint16_t)(g.op.result_len - 2U), g.op.result);
            finish(SUCCEEDED, E_NONE, 0);
        } else if (length > sizeof(g.op.result) - g.op.result_len) {
            finish(FAILED, E_LENGTH, 0);
        } else {
            memcpy(g.op.result + g.op.result_len, data, length);
            g.op.result_len += length;
            next = BT_GATT_ITER_CONTINUE;
        }
    }
    k_mutex_unlock(&gatt_lock);
    return next;
}

static void write_cb(struct bt_conn *conn, uint8_t err,
                     struct bt_gatt_write_params *params)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn == g.conn && params == &g.write && running() &&
        g.op.phase == WRITE_WAIT) {
        finish_att(err, true);
    }
    k_mutex_unlock(&gatt_lock);
}

static struct subscription *subscription_from_params(struct bt_gatt_subscribe_params *p)
{
    for (size_t i = 0; i < SUB_COUNT; ++i) {
        if (p == &g.subscriptions[i].params) { return &g.subscriptions[i]; }
    }
    return NULL;
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                        const void *data, uint16_t length)
{
    uint8_t record[24U + VALUE_MAX];
    void (*cb)(const uint8_t *, size_t) = NULL;
    k_mutex_lock(&gatt_lock, K_FOREVER);
    struct subscription *sub = subscription_from_params(params);
    if (sub && sub->used) {
        if (!data) { sub->active = false; sub->removed = true; }
        else if (conn == g.conn && g.conn_state == CONNECTED &&
                 length <= VALUE_MAX && length <= bt_gatt_get_mtu(conn) - 3U) {
            sys_put_le64((uint64_t)k_uptime_get(), record);
            sys_put_le32(sub->conn_id, record + 8);
            record[12] = sub->addr_type;
            memcpy(record + 13, sub->addr, 6);
            sys_put_le16(params->value_handle, record + 19);
            record[21] = sub->kind;
            sys_put_le16(length, record + 22);
            memcpy(record + 24, data, length);
            cb = g.record_cb;
        }
    }
    k_mutex_unlock(&gatt_lock);
    if (cb) { cb(record, 24U + length); }
    return BT_GATT_ITER_CONTINUE;
}

static void subscribe_cb(struct bt_conn *conn, uint8_t err,
                         struct bt_gatt_subscribe_params *params)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    struct subscription *sub = subscription_from_params(params);
    if (sub && sub->used && conn == g.conn) {
        bool unsubscribe = params->value == 0;
        sub->active = !err && !unsubscribe;
        if (err || unsubscribe) { sub->removed = true; }
        if (running() && g.op.slot >= 0 &&
            sub == &g.subscriptions[g.op.slot] &&
            (g.op.phase == SUB_WAIT || g.op.phase == UNSUB_WAIT)) {
            finish_att(err, false);
        }
    }
    k_mutex_unlock(&gatt_lock);
}

static void connected_result(void)
{
    sys_put_le32(g.conn_id, g.op.result);
    sys_put_le16(bt_gatt_get_mtu(g.conn), g.op.result + 4);
    g.op.result_len = 6;
    finish(SUCCEEDED, E_NONE, 0);
}

static void mtu_cb(struct bt_conn *conn, uint8_t err,
                   struct bt_gatt_exchange_params *params)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn == g.conn && params == &g.mtu && running() && g.op.phase == MTU_WAIT) {
        /* MTU exchange is optional. An ATT rejection leaves the valid default
         * MTU in force; connection success reports that actual negotiated MTU. */
        if (err == BT_ATT_ERR_UNLIKELY) {
            finish(FAILED, E_STACK, 0);
            g.close_needed = true;
        } else { connected_result(); }
    }
    k_mutex_unlock(&gatt_lock);
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn != g.conn) { k_mutex_unlock(&gatt_lock); return; }
    g.radio_complete = true;
    if (err) {
        g.conn = NULL;
        g.conn_id = 0;
        g.conn_state = NONE;
        g.draining = true;
        finish(FAILED, E_LINK, 0);
        bt_conn_unref(conn);
    } else {
        struct bt_conn_info info;
        g.conn_state = CONNECTED;
        if (!bt_conn_get_info(conn, &info) && info.le.remote) {
            wire_addr(info.le.remote, &g.addr_type, g.addr);
        }
        if (!running() || g.op.opcode != 0x10) { g.close_needed = true; }
        else { g.op.phase = g.op.request[17] ? SECURITY_START : MTU_START; }
    }
    k_mutex_unlock(&gatt_lock);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(reason);
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn == g.conn) {
        if (running()) {
            if (g.op.opcode == 0x11) { finish(SUCCEEDED, E_NONE, 0); }
            else { finish(g.op.sent && g.op.opcode == 0x14 ? UNKNOWN : FAILED, E_LINK, 0); }
        }
        g.conn = NULL;
        g.conn_id = 0;
        g.conn_state = NONE;
        g.draining = true;
        g.close_needed = false;
        g.radio_complete = true;
        memset(g.handles, 0, sizeof(g.handles));
        bt_conn_unref(conn);
    }
    k_mutex_unlock(&gatt_lock);
}

static void recycled_cb(void)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    /* CONFIG_BT_MAX_CONN=1: recycle is the lifetime barrier for every ATT
     * parameter and subscription from the previous connection generation. */
    if (!g.conn && g.draining) {
        memset(g.subscriptions, 0, sizeof(g.subscriptions));
        g.draining = false;
        g.close_needed = false;
    }
    k_mutex_unlock(&gatt_lock);
}

static void security_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (conn == g.conn && running() && g.op.phase == SECURITY_WAIT) {
        if (err || level < BT_SECURITY_L2) {
            finish(FAILED, E_SECURITY, 0);
            g.close_needed = true;
        } else { g.op.phase = MTU_START; }
    }
    k_mutex_unlock(&gatt_lock);
}

static struct bt_conn_cb connection_callbacks = {
    .connected = connected_cb, .disconnected = disconnected_cb,
    .recycled = recycled_cb, .security_changed = security_cb,
};

#if defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
static enum bt_security_err reject_pairing(struct bt_conn *conn,
                                          const struct bt_conn_pairing_feat *const feat)
{
    ARG_UNUSED(conn); ARG_UNUSED(feat);
    /* Provisioning is a separate authenticated manufacturing operation.
     * Never turn a failed bond re-encryption into opportunistic pairing. */
    return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}
static const struct bt_conn_auth_cb auth_callbacks = { .pairing_accept = reject_pairing };
#endif

struct bond_lookup { bt_addr_le_t addr; bool found; };
static void bond_cb(const struct bt_bond_info *info, void *user_data)
{
    struct bond_lookup *lookup = user_data;
    if (bt_addr_le_eq(&info->addr, &lookup->addr)) { lookup->found = true; }
}

int gw_gatt_init(void (*record_cb)(const uint8_t *, size_t))
{
    if (g.initialized) { return -EALREADY; }
#if !defined(CONFIG_BT_SMP_APP_PAIRING_ACCEPT)
    return -ENOTSUP;
#else
    int err = bt_conn_auth_cb_register(&auth_callbacks);
    if (err) { return err; }
    g.record_cb = record_cb;
    bt_set_bondable(false);
    bt_conn_cb_register(&connection_callbacks);
    g.initialized = true;
    return 0;
#endif
}

static int slot_find(uint16_t value, uint16_t ccc)
{
    for (size_t i = 0; i < SUB_COUNT; ++i) {
        if (g.subscriptions[i].used && !g.subscriptions[i].removed &&
            g.subscriptions[i].params.value_handle == value &&
            g.subscriptions[i].params.ccc_handle == ccc) { return (int)i; }
    }
    return -1;
}

static uint8_t validate(uint8_t opcode, const uint8_t *p, size_t len)
{
    static const uint8_t lengths[] = { 18, 14, 46, 16, 0, 19, 18, 0, 30 };
    if (opcode < 0x10 || opcode > 0x18 || opcode == 0x17) { return UNSUPPORTED; }
    if (opcode == 0x14) {
        if (len < 19 || len > REQUEST_MAX || sys_get_le16(p + 16) != len - 18) { return BAD_PARAM; }
    } else if (len != lengths[opcode - 0x10]) { return BAD_PARAM; }
    if (sys_get_le16(p + 8) < 100 || sys_get_le16(p + 8) > 30000) { return BAD_PARAM; }
    if (opcode == 0x10) {
        if (p[10] > 1 || p[17] > 1) { return BAD_PARAM; }
        if (g.conn || g.conn_state != NONE || g.next_conn_id == UINT32_MAX) { return BUSY; }
        return OK;
    }
    if (g.conn_state != CONNECTED || !g.conn || sys_get_le32(p + 10) != g.conn_id) { return NOT_FOUND; }
    if (opcode == 0x11 || opcode == 0x12 || opcode == 0x18) { return OK; }
    uint16_t value = sys_get_le16(p + 14);
    struct handle_entry *h = value ? handle_find(value) : NULL;
    if (!h) { return NOT_FOUND; }
    if (opcode == 0x13 && !(h->properties & BT_GATT_CHRC_READ)) { return BAD_PARAM; }
    if (opcode == 0x14 && (!(h->properties & BT_GATT_CHRC_WRITE) ||
        len - 18U > MIN(VALUE_MAX, bt_gatt_get_mtu(g.conn) - 3U))) { return BAD_PARAM; }
    if (opcode == 0x15 || opcode == 0x16) {
        if (!h->ccc || sys_get_le16(p + 16) != h->ccc) { return BAD_PARAM; }
        int slot = slot_find(value, h->ccc);
        if (opcode == 0x16) { return slot < 0 ? NOT_FOUND : OK; }
        if (p[18] < 1 || p[18] > 2 ||
            !(h->properties & (p[18] == 1 ? BT_GATT_CHRC_NOTIFY : BT_GATT_CHRC_INDICATE))) { return BAD_PARAM; }
        if (slot >= 0) { return g.subscriptions[slot].kind == p[18] - 1 ? OK : CONFLICT; }
        for (size_t i = 0; i < SUB_COUNT; ++i) {
            if (!g.subscriptions[i].used || g.subscriptions[i].removed) { return OK; }
        }
        return BUSY;
    }
    return OK;
}

uint8_t gw_gatt_command(uint8_t opcode, const uint8_t *args, size_t len,
                       uint8_t *out, size_t *out_len)
{
    uint8_t status = OK;
    if (!out_len || !out || (!args && len)) { return BAD_PARAM; }
    *out_len = 0;
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (!g.initialized) { status = APPLY_FAILED; goto done; }
    if (opcode == 0x0A) {
        if (len != 4) { status = BAD_PARAM; goto done; }
        if (g.conn_state != CONNECTED || sys_get_le32(args) != g.conn_id) { status = NOT_FOUND; goto done; }
        sys_put_le32(g.conn_id, out); out[4] = g.addr_type;
        memcpy(out + 5, g.addr, 6); sys_put_le16(bt_gatt_get_mtu(g.conn), out + 11);
        bt_security_t sec = bt_conn_get_security(g.conn);
        out[13] = sec >= BT_SECURITY_L3 ? 2 : sec >= BT_SECURITY_L2 ? 1 : 0;
        *out_len = 14; goto done;
    }
    if (opcode == 0x17) {
        if (len != 8) { status = BAD_PARAM; goto done; }
        uint64_t seq = sys_get_le64(args);
        if (!seq || seq != g.op.seq) { status = g.op.seq && seq < g.op.seq ? STALE : NOT_FOUND; goto done; }
        sys_put_le64(seq, out); out[8] = g.op.opcode; out[9] = g.op.state;
        out[10] = g.op.error; out[11] = g.op.att_error;
        *out_len = 12;
        if (g.op.state == SUCCEEDED) { memcpy(out + 12, g.op.result, g.op.result_len); *out_len += g.op.result_len; }
        goto done;
    }
    if (len < 10 || len > REQUEST_MAX) { status = BAD_PARAM; goto done; }
    uint64_t seq = sys_get_le64(args);
    if (!seq) { status = BAD_PARAM; goto done; }
    if (seq == g.op.seq) {
        if (opcode != g.op.opcode || len != g.op.request_len || memcmp(args, g.op.request, len)) { status = CONFLICT; goto done; }
        sys_put_le64(seq, out); *out_len = 8; goto done;
    }
    if (seq < g.op.seq) { status = STALE; goto done; }
    if (running() || g.draining || g.launching || g.close_needed ||
        g.conn_state == CLOSING) { status = BUSY; goto done; }
    status = validate(opcode, args, len);
    if (status) { goto done; }
    if (opcode == 0x10) {
        /* Reserve the radio before accepting this sequence. A conflicting
         * advertiser therefore returns BUSY without consuming op_seq. */
        g.launching = true;
        k_mutex_unlock(&gatt_lock);
        int err = gw_radio_prepare_connect();
        k_mutex_lock(&gatt_lock, K_FOREVER);
        g.launching = false;
        if (err) { status = err == -EBUSY ? BUSY : APPLY_FAILED; goto done; }
    }
    memset(&g.op, 0, sizeof(g.op));
    g.op.seq = seq; g.op.opcode = opcode;
    memcpy(g.op.request, args, len); g.op.request_len = len;
    g.op.deadline = k_uptime_get() + sys_get_le16(args + 8);
    g.op.slot = -1;
    switch (opcode) {
    case 0x10:
        g.conn_id = ++g.next_conn_id; g.conn_state = INITIATING;
        g.op.phase = CONNECT_START; break;
    case 0x11: g.op.phase = DISCONNECT_START; break;
    case 0x12: case 0x18: g.op.phase = SERVICE_START; break;
    case 0x13: g.op.phase = READ_START; break;
    case 0x14: g.op.phase = WRITE_START; break;
    case 0x15: g.op.phase = SUB_START; break;
    case 0x16: g.op.phase = UNSUB_START; break;
    default: break;
    }
    sys_put_le64(seq, out); *out_len = 8;
done:
    k_mutex_unlock(&gatt_lock);
    return status;
}

void gw_gatt_poll(void)
{
    int err = 0;
    k_mutex_lock(&gatt_lock, K_FOREVER);
    if (g.launching) { k_mutex_unlock(&gatt_lock); return; }
    if (g.radio_complete) {
        g.radio_complete = false;
        k_mutex_unlock(&gatt_lock);
        gw_radio_connect_complete();
        k_mutex_lock(&gatt_lock, K_FOREVER);
    }
    if (running() && k_uptime_get() >= g.op.deadline) {
        finish(g.op.sent && g.op.opcode == 0x14 ? UNKNOWN : TIMEOUT, E_DEADLINE, 0);
        /* Disconnect is a bounded cancellation barrier; params stay untouched
         * until recycled. Never reuse an ATT write buffer after timeout. */
        if (g.conn) { g.close_needed = true; }
    }
    if (g.close_needed && g.conn) {
        struct bt_conn *conn = bt_conn_ref(g.conn);
        g.conn_state = CLOSING;
        g.launching = true;
        k_mutex_unlock(&gatt_lock);
        err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        k_mutex_lock(&gatt_lock, K_FOREVER);
        if (!err) { g.close_needed = false; }
        g.launching = false;
        bt_conn_unref(conn);
        k_mutex_unlock(&gatt_lock);
        return;
    }
    if (!running() || g.draining) { k_mutex_unlock(&gatt_lock); return; }
    enum phase phase = g.op.phase;
    const uint8_t *p = g.op.request;
    if (phase == CONNECT_START) {
        struct bond_lookup lookup = { .addr.type = p[10] };
        for (size_t i = 0; i < 6; ++i) { lookup.addr.a.val[i] = p[16U - i]; }
        g.launching = true;
        wire_addr(&lookup.addr, &g.addr_type, g.addr);
        /* Creation publishes the returned reference before the callback
         * acquires this mutex. No ATT callback is needed to create it. */
        err = bt_conn_le_create(&lookup.addr, BT_CONN_LE_CREATE_CONN,
                                BT_LE_CONN_PARAM_DEFAULT, &g.conn);
        if (err) { g.conn_id = 0; g.conn_state = NONE; g.radio_complete = true; }
        else { g.op.phase = IDLE; }
        g.launching = false;
        if (err) { fail_stack(err); }
        k_mutex_unlock(&gatt_lock);
        return;
    }
    if (!g.conn || g.conn_state != CONNECTED) { k_mutex_unlock(&gatt_lock); return; }
    struct bt_conn *conn = bt_conn_ref(g.conn);
    void *params = NULL;
    switch (phase) {
    case SECURITY_START: {
        /* The observed address may be an RPA. Check the stack's resolved
         * identity after connection, rather than rejecting a known bond just
         * because its over-the-air address has rotated. */
        struct bond_lookup lookup = { .addr = *bt_conn_get_dst(conn) };
        bt_foreach_bond(BT_ID_DEFAULT, bond_cb, &lookup);
        if (!lookup.found) {
            finish(FAILED, E_SECURITY, 0); g.close_needed = true; goto no_launch;
        }
        if (bt_conn_get_security(conn) >= BT_SECURITY_L2) { g.op.phase = MTU_START; goto no_launch; }
        g.op.phase = SECURITY_WAIT; break;
    }
    case MTU_START:
        g.mtu = (struct bt_gatt_exchange_params){ .func = mtu_cb };
        g.op.phase = MTU_WAIT; break;
    case DISCONNECT_START: g.op.phase = DISCONNECT_WAIT; g.conn_state = CLOSING; break;
    case SERVICE_START:
        uuid_from_wire(&g.service_uuid, p + 14);
        if (g.op.opcode == 0x12) { uuid_from_wire(&g.char_uuid, p + 30); }
        g.matches = 0;
        g.discover = (struct bt_gatt_discover_params){ .uuid = &g.service_uuid.uuid,
            .func = discover_cb, .start_handle = 1, .end_handle = 0xFFFF,
            .type = BT_GATT_DISCOVER_PRIMARY };
        g.op.phase = SERVICE_WAIT; break;
    case CHAR_START:
        if (g.service_start >= g.service_end) { finish(FAILED, E_NOT_DISCOVERED, 0); goto no_launch; }
        g.matches = 0; g.value = 0; g.ccc = 0; g.declaration = 0;
        g.descriptor_end = g.service_end;
        g.discover = (struct bt_gatt_discover_params){ .func = discover_cb,
            .start_handle = g.service_start + 1U, .end_handle = g.service_end,
            .type = BT_GATT_DISCOVER_CHARACTERISTIC };
        g.op.phase = CHAR_WAIT; break;
    case CCC_START:
        g.matches = 0;
        g.discover = (struct bt_gatt_discover_params){ .uuid = BT_UUID_GATT_CCC,
            .func = discover_cb, .start_handle = g.value + 1U,
            .end_handle = g.descriptor_end, .type = BT_GATT_DISCOVER_DESCRIPTOR };
        g.op.phase = CCC_WAIT; break;
    case READ_START:
        g.read = (struct bt_gatt_read_params){ .func = read_cb, .handle_count = 1,
            .single = { .handle = sys_get_le16(p + 14), .offset = 0 } };
        g.op.result_len = 2; g.op.phase = READ_WAIT; break;
    case WRITE_START:
        g.write = (struct bt_gatt_write_params){ .func = write_cb,
            .handle = sys_get_le16(p + 14), .offset = 0,
            .length = sys_get_le16(p + 16), .data = p + 18 };
        g.op.phase = WRITE_WAIT; g.op.sent = true; break;
    case SUB_START: case UNSUB_START: {
        int slot = slot_find(sys_get_le16(p + 14), sys_get_le16(p + 16));
        if (phase == SUB_START && slot >= 0) { finish(SUCCEEDED, E_NONE, 0); goto no_launch; }
        if (phase == SUB_START) {
            for (size_t i = 0; i < SUB_COUNT; ++i) {
                if (!g.subscriptions[i].used || g.subscriptions[i].removed) { slot = (int)i; break; }
            }
            if (slot < 0) { finish(FAILED, E_STACK, 0); goto no_launch; }
            struct subscription *sub = &g.subscriptions[slot];
            memset(sub, 0, sizeof(*sub));
            sub->used = true; sub->conn_id = g.conn_id;
            sub->addr_type = g.addr_type; memcpy(sub->addr, g.addr, 6); sub->kind = p[18] - 1U;
            sub->params = (struct bt_gatt_subscribe_params){ .notify = notify_cb,
                .subscribe = subscribe_cb, .value_handle = sys_get_le16(p + 14),
                .ccc_handle = sys_get_le16(p + 16), .value = p[18] };
            atomic_set_bit(sub->params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
            atomic_set_bit(sub->params.flags, BT_GATT_SUBSCRIBE_FLAG_NO_RESUB);
        }
        if (slot < 0) { finish(FAILED, E_NOT_DISCOVERED, 0); goto no_launch; }
        g.op.slot = slot; params = &g.subscriptions[slot].params;
        g.op.phase = phase == SUB_START ? SUB_WAIT : UNSUB_WAIT;
        break;
    }
    default: goto no_launch;
    }
    /* Stack calls may wait for ATT TX resources. Do not hold the application
     * mutex while a Bluetooth callback could be needed to free those resources. */
    g.launching = true;
    k_mutex_unlock(&gatt_lock);
    switch (phase) {
    case SECURITY_START: err = bt_conn_set_security(conn, BT_SECURITY_L2); break;
    case MTU_START: err = bt_gatt_exchange_mtu(conn, &g.mtu); break;
    case DISCONNECT_START: err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN); break;
    case SERVICE_START: case CHAR_START: case CCC_START: err = bt_gatt_discover(conn, &g.discover); break;
    case READ_START: err = bt_gatt_read(conn, &g.read); break;
    case WRITE_START: err = bt_gatt_write(conn, &g.write); break;
    case SUB_START: err = bt_gatt_subscribe(conn, params); break;
    case UNSUB_START: err = bt_gatt_unsubscribe(conn, params); break;
    default: break;
    }
    k_mutex_lock(&gatt_lock, K_FOREVER);
    g.launching = false;
    if (err && running()) {
        g.op.sent = false;
        if (phase == MTU_START && err == -EALREADY) { connected_result(); }
        else {
            if (phase == SUB_START) { g.subscriptions[g.op.slot].removed = true; }
            if (phase == DISCONNECT_START && g.conn) { g.conn_state = CONNECTED; }
            if (phase == SECURITY_START || phase == MTU_START) { g.close_needed = true; }
            fail_stack(err);
        }
    }
no_launch:
    bt_conn_unref(conn);
    k_mutex_unlock(&gatt_lock);
}

uint32_t gw_gatt_connection_id(void)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    uint32_t id = g.conn_id;
    k_mutex_unlock(&gatt_lock);
    return id;
}

uint8_t gw_gatt_connection_state(void)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    uint8_t state = g.conn_state;
    k_mutex_unlock(&gatt_lock);
    return state;
}

void gw_gatt_connection_snapshot(uint32_t *id, uint8_t *state)
{
    k_mutex_lock(&gatt_lock, K_FOREVER);
    *id = g.conn_id;
    *state = g.conn_state;
    k_mutex_unlock(&gatt_lock);
}
