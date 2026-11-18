#ifndef RADIO_MOCK_H
#define RADIO_MOCK_H
/* Host-only API facade. Never add this include directory to firmware builds. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_GW_RADIO_VERIFIED 1
#define CONFIG_SETTINGS 1
#define CONFIG_BT_MAX_PAIRED 4
#define IS_ENABLED(x) (x)
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) ((void)(x))
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define K_MUTEX_DEFINE(name) int name
static inline int k_mutex_lock(int *lock, int timeout) { (void)lock; (void)timeout; return 0; }
static inline void k_mutex_unlock(int *lock) { (void)lock; }
extern int64_t radio_mock_now;
static inline int64_t k_uptime_get(void) { return radio_mock_now; }
typedef int atomic_t;
static inline int atomic_get(const atomic_t *p) { return *p; }
static inline int atomic_set(atomic_t *p, int value) { int old = *p; *p = value; return old; }
static inline void atomic_clear(atomic_t *p) { *p = 0; }
static inline void atomic_inc(atomic_t *p) { ++*p; }
typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1
#define BT_ID_DEFAULT 0
static inline bool bt_addr_le_eq(const bt_addr_le_t *a, const bt_addr_le_t *b)
{ return a->type == b->type && memcmp(a->a.val, b->a.val, 6) == 0; }
struct bt_bond_info { bt_addr_le_t addr; };
struct bt_conn { int unused; };
#define BT_CONN_TYPE_LE 1
void bt_conn_foreach(int type, void (*callback)(struct bt_conn *, void *), void *user);
struct net_buf_simple { uint8_t *data; uint16_t len; };
struct bt_le_scan_recv_info { const bt_addr_le_t *addr; uint16_t adv_props; int8_t rssi; };
struct bt_le_scan_cb {
    void (*recv)(const struct bt_le_scan_recv_info *, struct net_buf_simple *);
};
struct bt_le_scan_param { uint8_t type; uint32_t options; uint16_t interval, window; };
struct bt_le_adv_param { uint8_t id; uint32_t options; uint16_t interval_min, interval_max; };
struct bt_data { uint8_t type, data_len; const uint8_t *data; };
#define BT_LE_SCAN_TYPE_PASSIVE 0
#define BT_LE_SCAN_TYPE_ACTIVE 1
#define BT_LE_SCAN_OPT_NONE 0
#define BT_LE_ADV_OPT_NONE 0
#define BT_LE_ADV_OPT_SCANNABLE BIT(9)
#define BT_LE_ADV_OPT_CONN (BIT(0) | BIT(1))
#define BT_GAP_ADV_PROP_CONNECTABLE BIT(0)
#define BT_GAP_ADV_PROP_SCANNABLE BIT(1)
#define BT_GAP_ADV_PROP_DIRECTED BIT(2)
#define BT_GAP_ADV_PROP_SCAN_RESPONSE BIT(3)
#define BT_GAP_ADV_PROP_EXT_ADV BIT(4)
int bt_enable(void *callback);
int settings_load(void);
void bt_foreach_bond(uint8_t id, void (*callback)(const struct bt_bond_info *, void *), void *user);
void bt_le_scan_cb_register(struct bt_le_scan_cb *callback);
int bt_le_scan_start(const struct bt_le_scan_param *params, void *callback);
int bt_le_scan_stop(void);
int bt_le_adv_start(const struct bt_le_adv_param *params, const struct bt_data *ad,
                    size_t ad_count, const struct bt_data *rsp, size_t rsp_count);
int bt_le_adv_stop(void);
int bt_le_adv_update_data(const struct bt_data *ad, size_t ad_count,
                          const struct bt_data *rsp, size_t rsp_count);
#endif
