#ifndef GATT_MOCK_H
#define GATT_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_BT_SMP_APP_PAIRING_ACCEPT 1
#define CONFIG_BT_SMP 1
#define K_FOREVER 0
#define K_MUTEX_DEFINE(n) int n
#define ARG_UNUSED(x) (void)(x)
#define MIN(a,b) ((a)<(b)?(a):(b))
static int64_t mock_time;
static int mock_lock_depth;
static inline int k_mutex_lock(int *m, int t) { (void)m; (void)t; ++mock_lock_depth; return 0; }
static inline int k_mutex_unlock(int *m) { (void)m; --mock_lock_depth; return 0; }
static inline int64_t k_uptime_get(void) { return mock_time; }
static inline uint16_t sys_get_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static inline uint32_t sys_get_le32(const uint8_t *p) { return sys_get_le16(p) | (uint32_t)sys_get_le16(p+2)<<16; }
static inline uint64_t sys_get_le64(const uint8_t *p) { return sys_get_le32(p) | (uint64_t)sys_get_le32(p+4)<<32; }
static inline void sys_put_le16(uint16_t v,uint8_t *p) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static inline void sys_put_le32(uint32_t v,uint8_t *p) { sys_put_le16((uint16_t)v,p); sys_put_le16((uint16_t)(v>>16),p+2); }
static inline void sys_put_le64(uint64_t v,uint8_t *p) { sys_put_le32((uint32_t)v,p); sys_put_le32((uint32_t)(v>>32),p+4); }
static inline void atomic_set_bit(unsigned int *p,unsigned int bit) { *p|=1U<<bit; }
typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
static inline bool bt_addr_le_eq(const bt_addr_le_t *a,const bt_addr_le_t *b) { return !memcmp(a,b,sizeof(*a)); }
struct bt_uuid { uint8_t type; };
struct bt_uuid_128 { struct bt_uuid uuid; uint8_t val[16]; };
static struct bt_uuid mock_ccc = {1};
#define BT_UUID_TYPE_128 2
#define BT_UUID_GATT_CCC (&mock_ccc)
static inline int bt_uuid_cmp(const struct bt_uuid *a,const struct bt_uuid *b) {
    if (a->type != b->type) { return 1; }
    return a->type==2 ? memcmp(((const struct bt_uuid_128 *)a)->val,((const struct bt_uuid_128 *)b)->val,16) : 0;
}
typedef unsigned int bt_security_t;
enum bt_security_err { BT_SECURITY_ERR_PAIR_NOT_ALLOWED=5 };
#define BT_SECURITY_L2 2
#define BT_SECURITY_L3 3
#define BT_ATT_ERR_UNLIKELY 0x0e
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_ID_DEFAULT 0
struct bt_conn { int refs; };
static struct bt_conn mock_conn;
static bt_addr_le_t mock_peer;
struct bt_conn_info { struct { const bt_addr_le_t *remote; } le; };
struct bt_conn_pairing_feat { int unused; };
struct bt_bond_info { bt_addr_le_t addr; };
struct bt_conn_auth_cb { enum bt_security_err (*pairing_accept)(struct bt_conn *,const struct bt_conn_pairing_feat *const); };
struct bt_conn_cb {
    void (*connected)(struct bt_conn *,uint8_t);
    void (*disconnected)(struct bt_conn *,uint8_t);
    void (*recycled)(void);
    void (*security_changed)(struct bt_conn *,bt_security_t,enum bt_security_err);
};
static struct bt_conn_cb *mock_callbacks;
static unsigned mock_disconnects, mock_writes, mock_subscribes, mock_unsubscribes;
static bool mock_bond;
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { ++c->refs; return c; }
static inline void bt_conn_unref(struct bt_conn *c) { if (!--c->refs && mock_callbacks) { mock_callbacks->recycled(); } }
static inline void bt_conn_cb_register(struct bt_conn_cb *cb) { mock_callbacks=cb; }
static inline int bt_conn_auth_cb_register(const struct bt_conn_auth_cb *cb) { (void)cb; return 0; }
static inline void bt_set_bondable(bool b) { (void)b; }
static inline int bt_conn_get_info(struct bt_conn *c,struct bt_conn_info *info) { (void)c; info->le.remote=&mock_peer; return 0; }
static inline const bt_addr_le_t *bt_conn_get_dst(struct bt_conn *c) { (void)c; return &mock_peer; }
static inline bt_security_t bt_conn_get_security(struct bt_conn *c) { (void)c; return 1; }
static inline int bt_conn_set_security(struct bt_conn *c,bt_security_t s) { (void)c; (void)s; return 0; }
static inline void bt_foreach_bond(uint8_t id,void (*cb)(const struct bt_bond_info *,void *),void *u) {
    (void)id; struct bt_bond_info info={mock_peer}; if (mock_bond) { cb(&info,u); }
}
#define BT_CONN_LE_CREATE_CONN NULL
#define BT_LE_CONN_PARAM_DEFAULT NULL
static inline int bt_conn_le_create(const bt_addr_le_t *a,const void *p,const void *q,struct bt_conn **out) {
    (void)p; (void)q; mock_peer=*a; mock_conn.refs=1; *out=&mock_conn; return 0;
}
static inline int bt_conn_disconnect(struct bt_conn *c,uint8_t r) { (void)c; (void)r; ++mock_disconnects; return 0; }

#define BT_GATT_ITER_STOP 0
#define BT_GATT_ITER_CONTINUE 1
#define BT_GATT_DISCOVER_PRIMARY 0
#define BT_GATT_DISCOVER_CHARACTERISTIC 1
#define BT_GATT_DISCOVER_DESCRIPTOR 2
#define BT_GATT_CHRC_READ 0x02
#define BT_GATT_CHRC_WRITE 0x08
#define BT_GATT_CHRC_NOTIFY 0x10
#define BT_GATT_CHRC_INDICATE 0x20
#define BT_GATT_SUBSCRIBE_FLAG_VOLATILE 0
#define BT_GATT_SUBSCRIBE_FLAG_NO_RESUB 1
struct bt_gatt_attr { uint16_t handle; const void *user_data; };
struct bt_gatt_service_val { uint16_t end_handle; struct bt_uuid *uuid; };
struct bt_gatt_chrc { struct bt_uuid *uuid; uint16_t value_handle; uint8_t properties; };
struct bt_gatt_discover_params {
    const struct bt_uuid *uuid;
    uint8_t (*func)(struct bt_conn *,const struct bt_gatt_attr *,struct bt_gatt_discover_params *);
    uint16_t start_handle,end_handle; uint8_t type;
};
struct bt_gatt_read_params {
    uint8_t (*func)(struct bt_conn *,uint8_t,struct bt_gatt_read_params *,const void *,uint16_t);
    uint8_t handle_count;
    struct { uint16_t handle,offset; } single;
};
struct bt_gatt_write_params {
    void (*func)(struct bt_conn *,uint8_t,struct bt_gatt_write_params *);
    uint16_t handle,offset,length; const void *data;
};
struct bt_gatt_exchange_params { void (*func)(struct bt_conn *,uint8_t,struct bt_gatt_exchange_params *); };
struct bt_gatt_subscribe_params {
    uint8_t (*notify)(struct bt_conn *,struct bt_gatt_subscribe_params *,const void *,uint16_t);
    void (*subscribe)(struct bt_conn *,uint8_t,struct bt_gatt_subscribe_params *);
    uint16_t value_handle,ccc_handle,value;
    unsigned int flags[1];
};
static inline uint16_t bt_gatt_get_mtu(struct bt_conn *c) { (void)c; return 247; }
static inline int bt_gatt_exchange_mtu(struct bt_conn *c,struct bt_gatt_exchange_params *p) { (void)c; (void)p; return 0; }
static inline int bt_gatt_discover(struct bt_conn *c,struct bt_gatt_discover_params *p) { (void)c; (void)p; return 0; }
static inline int bt_gatt_read(struct bt_conn *c,struct bt_gatt_read_params *p) { (void)c; (void)p; return 0; }
static inline int bt_gatt_write(struct bt_conn *c,struct bt_gatt_write_params *p) { (void)c; (void)p; ++mock_writes; return 0; }
static inline int bt_gatt_subscribe(struct bt_conn *c,struct bt_gatt_subscribe_params *p) { (void)c; (void)p; ++mock_subscribes; return 0; }
static inline int bt_gatt_unsubscribe(struct bt_conn *c,struct bt_gatt_subscribe_params *p) { (void)c; p->value=0; ++mock_unsubscribes; return 0; }
#endif
