#ifndef APP_MOCK_ZEPHYR_H
#define APP_MOCK_ZEPHYR_H
/* Deliberately small single-threaded behavior facade, not a Zephyr ABI model. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define ARG_UNUSED(x) (void)(x)
#define BIT(n) (1UL << (n))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define __aligned(n) __attribute__((aligned(n)))
#define K_MSEC(n) (n)
#define K_FOREVER (-1)
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
extern int64_t mock_now;
static inline int64_t k_uptime_get(void) { return mock_now; }
static inline void k_sleep(int ms) { mock_now += ms; }
struct k_sem { int count; };
#define K_SEM_DEFINE(name, initial, limit) struct k_sem name = {initial}
static inline void k_sem_reset(struct k_sem *s) { s->count = 0; }
static inline void k_sem_give(struct k_sem *s) { s->count = 1; }
static inline int k_sem_take(struct k_sem *s, int timeout) {
    ARG_UNUSED(timeout); if (!s->count) return -EAGAIN; s->count = 0; return 0;
}
struct k_mutex { int unused; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
static inline void k_mutex_lock(struct k_mutex *m, int t) { ARG_UNUSED(m); ARG_UNUSED(t); }
static inline void k_mutex_unlock(struct k_mutex *m) { ARG_UNUSED(m); }
typedef int atomic_t;
#define ATOMIC_INIT(n) (n)
static inline int atomic_get(atomic_t *a) { return *a; }
static inline int atomic_set(atomic_t *a, int v) { int old=*a; *a=v; return old; }
static inline void atomic_clear(atomic_t *a) { *a=0; }
static inline void atomic_inc(atomic_t *a) { ++*a; }
static inline bool atomic_cas(atomic_t *a,int old,int v) { if (*a!=old) return false; *a=v; return true; }
struct device { int unused; };
extern const struct device mock_device;
#define DT_ALIAS(n) n
#define DEVICE_DT_GET(n) (&mock_device)
static inline bool device_is_ready(const struct device *d) { ARG_UNUSED(d); return true; }
enum { UART_RX_RDY, UART_RX_BUF_REQUEST, UART_RX_BUF_RELEASED, UART_RX_DISABLED,
       UART_RX_STOPPED, UART_TX_DONE, UART_TX_ABORTED };
struct uart_event {
    int type;
    union {
        struct { uint8_t *buf; size_t offset, len; } rx;
        struct { uint8_t *buf; } rx_buf;
    } data;
};
enum { UART_CFG_PARITY_NONE, UART_CFG_STOP_BITS_1, UART_CFG_DATA_BITS_8,
       UART_CFG_FLOW_CTRL_RTS_CTS };
struct uart_config { uint32_t baudrate; int parity,stop_bits,data_bits,flow_ctrl; };
typedef void (*mock_uart_callback_t)(const struct device *, struct uart_event *, void *);
int uart_tx(const struct device *, const uint8_t *, size_t, int);
int uart_tx_abort(const struct device *);
int uart_callback_set(const struct device *, mock_uart_callback_t, void *);
static inline int uart_configure(const struct device *d,const struct uart_config *c) { ARG_UNUSED(d); ARG_UNUSED(c); return 0; }
static inline int uart_rx_enable(const struct device *d,uint8_t *b,size_t n,int t) { ARG_UNUSED(d); ARG_UNUSED(b); ARG_UNUSED(n); ARG_UNUSED(t); return 0; }
static inline int uart_rx_buf_rsp(const struct device *d,uint8_t *b,size_t n) { ARG_UNUSED(d); ARG_UNUSED(b); ARG_UNUSED(n); return 0; }
struct ring_buf { uint8_t *buf; size_t size, used; };
#define RING_BUF_DECLARE(name, n) static uint8_t name##_storage[n]; struct ring_buf name = {name##_storage,n,0}
static inline uint32_t ring_buf_put(struct ring_buf *r,const uint8_t *b,size_t n) { n=MIN(n,r->size-r->used); memcpy(r->buf+r->used,b,n); r->used+=n; return (uint32_t)n; }
static inline uint32_t ring_buf_get(struct ring_buf *r,uint8_t *b,size_t n) { n=MIN(n,r->used); memcpy(b,r->buf,n); r->used-=n; memmove(r->buf,r->buf+n,r->used); return (uint32_t)n; }
struct sys_notify { int unused; };
struct onoff_client { struct sys_notify notify; };
struct onoff_manager { int unused; };
#define CLOCK_CONTROL_NRF_SUBSYS_HF 0
static inline struct onoff_manager *z_nrf_clock_control_get_onoff(int n) { static struct onoff_manager m; ARG_UNUSED(n); return &m; }
static inline void sys_notify_init_spinwait(struct sys_notify *n) { ARG_UNUSED(n); }
static inline int onoff_request(struct onoff_manager *m,struct onoff_client *c) { ARG_UNUSED(m); ARG_UNUSED(c); return 0; }
static inline int onoff_cancel_or_release(struct onoff_manager *m,struct onoff_client *c) { ARG_UNUSED(m); ARG_UNUSED(c); return 0; }
static inline int sys_notify_fetch_result(struct sys_notify *n,int *r) { ARG_UNUSED(n); *r=0; return 0; }
static inline uint32_t sys_rand32_get(void) { return 0; }
static inline int sys_csrand_get(void *p,size_t n) { memset(p,0x42,n); return 0; }
#define SYS_REBOOT_COLD 0
extern unsigned mock_reboots;
static inline void sys_reboot(int n) { ARG_UNUSED(n); ++mock_reboots; }
#endif
