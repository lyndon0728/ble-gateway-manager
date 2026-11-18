#ifndef PERIPHERAL_MOCK_H
#define PERIPHERAL_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#define K_FOREVER 0
#define K_MUTEX_DEFINE(n) int n
#define ARG_UNUSED(x) (void)(x)
static int64_t mock_time;
static int mock_lock_depth;
static inline int k_mutex_lock(int *m,int timeout) { (void)m; (void)timeout; ++mock_lock_depth; return 0; }
static inline int k_mutex_unlock(int *m) { (void)m; --mock_lock_depth; return 0; }
static inline int64_t k_uptime_get(void) { return mock_time; }
static inline uint16_t sys_get_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static inline uint32_t sys_get_le32(const uint8_t *p) { return sys_get_le16(p) | (uint32_t)sys_get_le16(p+2)<<16; }
static inline uint64_t sys_get_le64(const uint8_t *p) { return sys_get_le32(p) | (uint64_t)sys_get_le32(p+4)<<32; }
static inline void sys_put_le16(uint16_t v,uint8_t *p) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void sys_put_le32(uint32_t v,uint8_t *p) { sys_put_le16((uint16_t)v,p); sys_put_le16((uint16_t)(v>>16),p+2); }
static inline void sys_put_le64(uint64_t v,uint8_t *p) { sys_put_le32((uint32_t)v,p); sys_put_le32((uint32_t)(v>>32),p+4); }
typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
struct bt_uuid { uint8_t type; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };
#define PM_BYTE(v,n) ((uint8_t)((uint64_t)(v) >> (n)))
#define PM_LE16(v) PM_BYTE(v,0),PM_BYTE(v,8)
#define BT_UUID_128_ENCODE(a,b,c,d,e) PM_BYTE(e,0),PM_BYTE(e,8),PM_BYTE(e,16),PM_BYTE(e,24),PM_BYTE(e,32),PM_BYTE(e,40),PM_LE16(d),PM_LE16(c),PM_LE16(b),PM_BYTE(a,0),PM_BYTE(a,8),PM_BYTE(a,16),PM_BYTE(a,24)
#define BT_UUID_INIT_128(...) { .uuid={.type=2},.val={__VA_ARGS__} }
#define BT_CONN_ROLE_CENTRAL 0
#define BT_CONN_ROLE_PERIPHERAL 1
#define BT_SECURITY_L2 2
#define BT_SECURITY_L3 3
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
typedef unsigned int bt_security_t;
struct bt_conn { int refs; uint8_t role,security; uint16_t mtu; };
struct bt_conn_info { uint8_t role; struct { const bt_addr_le_t *remote; } le; };
struct bt_conn_cb {
    void (*connected)(struct bt_conn *,uint8_t);
    void (*disconnected)(struct bt_conn *,uint8_t);
    void (*recycled)(void);
};
static struct bt_conn mock_conn;
static bt_addr_le_t mock_peer;
static struct bt_conn_cb *mock_callbacks;
static unsigned int mock_disconnects;
static int mock_disconnect_error;
static inline void bt_conn_cb_register(struct bt_conn_cb *cb) { mock_callbacks=cb; }
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { ++c->refs; return c; }
static inline void bt_conn_unref(struct bt_conn *c) { assert(c->refs>0); if (!--c->refs) { mock_callbacks->recycled(); } }
static inline int bt_conn_get_info(struct bt_conn *c,struct bt_conn_info *info) { info->role=c->role; info->le.remote=&mock_peer; return 0; }
static inline bt_security_t bt_conn_get_security(struct bt_conn *c) { return c->security; }
static inline int bt_conn_disconnect(struct bt_conn *c,uint8_t reason) {
    (void)c; (void)reason; assert(mock_lock_depth==0); ++mock_disconnects; return mock_disconnect_error;
}
#define BT_GATT_PERM_NONE 0
#define BT_GATT_PERM_READ 1
#define BT_GATT_PERM_WRITE 2
#define BT_GATT_CHRC_READ 0x02
#define BT_GATT_CHRC_WRITE 0x08
#define BT_GATT_CHRC_NOTIFY 0x10
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_WRITE_FLAG_PREPARE 1
#define BT_GATT_WRITE_FLAG_CMD 2
#define BT_GATT_WRITE_FLAG_EXECUTE 4
#define BT_ATT_ERR_WRITE_NOT_PERMITTED 0x03
#define BT_ATT_ERR_INVALID_OFFSET 0x07
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 0x0d
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 0x11
#define BT_GATT_ERR(e) (-(e))
struct bt_gatt_attr {
    const struct bt_uuid *uuid;
    uint8_t properties,perm;
    ssize_t (*read)(struct bt_conn *,const struct bt_gatt_attr *,void *,uint16_t,uint16_t);
    ssize_t (*write)(struct bt_conn *,const struct bt_gatt_attr *,const void *,uint16_t,uint16_t,uint8_t);
    void (*changed)(const struct bt_gatt_attr *,uint16_t);
};
#define BT_GATT_PRIMARY_SERVICE(_uuid) { .uuid=(_uuid) }
#define BT_GATT_CHARACTERISTIC(_uuid,_props,_perm,_read,_write,_data) { .properties=(_props) },{ .uuid=(_uuid),.perm=(_perm),.read=(_read),.write=(_write) }
#define BT_GATT_CCC(_changed,_perm) { .perm=(_perm),.changed=(_changed) }
#define BT_GATT_SERVICE_DEFINE(_name,...) static const struct bt_gatt_attr _name##_attrs[]={__VA_ARGS__}; static const struct { const struct bt_gatt_attr *attrs; } _name={_name##_attrs}
struct bt_gatt_notify_params {
    const struct bt_uuid *uuid;
    const struct bt_gatt_attr *attr;
    const void *data; uint16_t len;
    void (*func)(struct bt_conn *,void *);
    void *user_data;
};
static bool mock_subscribed, mock_complete_immediately;
static int mock_notify_error;
static unsigned int mock_notifies;
static struct bt_gatt_notify_params *mock_last_notify;
static void (*mock_notify_hook)(struct bt_conn *);
static inline int bt_gatt_notify_cb(struct bt_conn *c,struct bt_gatt_notify_params *p) {
    assert(mock_lock_depth==0); ++mock_notifies; mock_last_notify=p;
    if (!mock_notify_error && mock_complete_immediately) { p->func(c,p->user_data); }
    if (mock_notify_hook) { mock_notify_hook(c); }
    return mock_notify_error;
}
static inline uint16_t bt_gatt_get_mtu(struct bt_conn *c) { return c->mtu; }
static inline bool bt_gatt_is_subscribed(struct bt_conn *c,const struct bt_gatt_attr *a,uint16_t kind) {
    (void)c; (void)a; assert(kind==BT_GATT_CCC_NOTIFY); return mock_subscribed;
}
static inline ssize_t bt_gatt_attr_read(struct bt_conn *c,const struct bt_gatt_attr *a,
    void *out,uint16_t n,uint16_t off,const void *data,uint16_t size) {
    (void)c; (void)a;
    if (off>size) { return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET); }
    size-=off; if (n>size) { n=size; }
    memcpy(out,(const uint8_t *)data+off,n); return n;
}
#endif
