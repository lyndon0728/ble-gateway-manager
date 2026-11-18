#include "gw_core.h"
#include "gw_gatt.h"
#include "gw_peripheral.h"
#include "gw_dfu.h"
#include "gw_radio.h"
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gw_app, LOG_LEVEL_INF);

#define RX_DMA_SIZE 256
#define RX_DMA_COUNT 3
#define RX_RING_SIZE 8192
#define FW_VERSION "gw-nrf54l15/0.2.0"

static const struct device *const link_uart = DEVICE_DT_GET(DT_ALIAS(gw_uart));
static uint8_t rx_dma[RX_DMA_COUNT][RX_DMA_SIZE] __aligned(4);
static atomic_t rx_owned[RX_DMA_COUNT];
static atomic_t rx_disabled = ATOMIC_INIT(1);
static atomic_t rx_overflow;
static atomic_t rx_driver_errors;
static atomic_t tx_busy;
static atomic_t tx_success;
static uint8_t tx_wire[GW_WIRE_MAX] __aligned(4);
RING_BUF_DECLARE(rx_ring, RX_RING_SIZE);
K_SEM_DEFINE(tx_done, 0, 1);
K_MUTEX_DEFINE(queue_lock);

static struct gw_queue queue;
static struct gw_decoder decoder;
static struct gw_frame incoming;
static struct gw_frame outgoing;
static struct gw_frame cached_response;
static uint8_t cached_request[GW_PAYLOAD_MAX];
static uint16_t cached_request_length;
static uint32_t last_command;
static uint8_t boot_id[16];
static uint64_t link_id;
static bool link_established;
static uint64_t dropped_full, bad_rx, retries;
static uint64_t next_batch_send_ms;
static uint64_t ack_deadline_ms;
static uint32_t sent_batch;
static uint32_t retry_backoff_ms = 250;

static int hold_uart_hfxo(void)
{
    /* NCS v3.2.1 UARTE does not own the crystal request. Use the clock
     * manager shared with MPSL; never directly trigger clock hardware. */
    static struct onoff_client client;
    struct onoff_manager *manager =
        z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!manager) {
        return -ENODEV;
    }
    sys_notify_init_spinwait(&client.notify);
    int err = onoff_request(manager, &client);
    if (err < 0) {
        return err;
    }
    int result = 0;
    int64_t deadline = k_uptime_get() + 2000;
    while ((err = sys_notify_fetch_result(&client.notify, &result)) == -EAGAIN &&
           k_uptime_get() < deadline) {
        k_sleep(K_MSEC(1));
    }
    if (err < 0 || result < 0) {
        (void)onoff_cancel_or_release(manager, &client);
        return err < 0 ? err : result;
    }
    /* Continuous RX owns this reference until reboot. */
    return 0;
}

static int claim_rx_buffer(void)
{
    for (int i = 0; i < RX_DMA_COUNT; ++i) {
        if (atomic_cas(&rx_owned[i], 0, 1)) {
            return i;
        }
    }
    return -1;
}

static void uart_event(const struct device *dev, struct uart_event *event, void *unused)
{
    ARG_UNUSED(unused);
    switch (event->type) {
    case UART_RX_RDY: {
        const uint8_t *data = event->data.rx.buf + event->data.rx.offset;
        size_t length = event->data.rx.len;
        if (ring_buf_put(&rx_ring, data, length) != length) {
            atomic_inc(&rx_overflow);
        }
        break;
    }
    case UART_RX_BUF_REQUEST: {
        int slot = claim_rx_buffer();
        if (slot >= 0 && uart_rx_buf_rsp(dev, rx_dma[slot], RX_DMA_SIZE) != 0) {
            atomic_clear(&rx_owned[slot]);
        }
        break;
    }
    case UART_RX_BUF_RELEASED:
        for (int i = 0; i < RX_DMA_COUNT; ++i) {
            if (event->data.rx_buf.buf == rx_dma[i]) {
                atomic_clear(&rx_owned[i]);
                break;
            }
        }
        break;
    case UART_RX_DISABLED:
        atomic_set(&rx_disabled, 1);
        break;
    case UART_RX_STOPPED:
        atomic_inc(&rx_driver_errors);
        break;
    case UART_TX_DONE:
        atomic_set(&tx_success, 1);
        atomic_clear(&tx_busy);
        k_sem_give(&tx_done);
        break;
    case UART_TX_ABORTED:
        atomic_clear(&tx_success);
        atomic_clear(&tx_busy);
        k_sem_give(&tx_done);
        break;
    default:
        break;
    }
}

static void restart_rx(void)
{
    if (!atomic_cas(&rx_disabled, 1, 0)) {
        return;
    }
    int slot = claim_rx_buffer();
    if (slot < 0) {
        atomic_set(&rx_disabled, 1);
        return;
    }
    /* A short idle timeout delivers a partial command without awaiting 256 bytes. */
    int err = uart_rx_enable(link_uart, rx_dma[slot], RX_DMA_SIZE, 1000);
    if (err) {
        atomic_clear(&rx_owned[slot]);
        atomic_set(&rx_disabled, 1);
    }
}

static int send_frame(const struct gw_frame *frame)
{
    if (atomic_get(&tx_busy)) {
        return -EBUSY;
    }
    size_t length = gw_encode_frame(frame, tx_wire, sizeof(tx_wire));
    if (!length) {
        return -EINVAL;
    }
    k_sem_reset(&tx_done);
    atomic_clear(&tx_success);
    atomic_set(&tx_busy, 1);
    /* The DMA buffer is never reused until DONE or ABORTED releases ownership. */
    int err = uart_tx(link_uart, tx_wire, length, 200000);
    if (err) {
        atomic_clear(&tx_busy);
        return err;
    }
    if (k_sem_take(&tx_done, K_MSEC(300))) {
        (void)uart_tx_abort(link_uart);
        (void)k_sem_take(&tx_done, K_MSEC(100));
        return -ETIMEDOUT;
    }
    return atomic_get(&tx_success) ? 0 : -ETIMEDOUT;
}

bool gw_app_record(uint8_t type, const uint8_t *record, size_t length)
{
    k_mutex_lock(&queue_lock, K_FOREVER);
    bool accepted = gw_queue_push(&queue, type, record, length, k_uptime_get());
    if (!accepted) {
        dropped_full++;
    }
    k_mutex_unlock(&queue_lock);
    return accepted;
}

static void gatt_record(const uint8_t *record, size_t length)
{
    (void)gw_app_record(0x21, record, length);
}

static size_t encode_status(uint8_t *out)
{
    uint64_t seen, filtered, incomplete;
    gw_radio_counters(&seen, &filtered, &incomplete);
    gw_put_u64(out, k_uptime_get());
    gw_put_u64(out + 8, gw_radio_revision());
    out[16] = gw_radio_scan_state();
    k_mutex_lock(&queue_lock, K_FOREVER);
    gw_put_u16(out + 17, queue.count);
    gw_put_u32(out + 19, queue.used_bytes);
    gw_put_u64(out + 39, dropped_full);
    k_mutex_unlock(&queue_lock);
    gw_put_u64(out + 23, seen);
    gw_put_u64(out + 31, filtered);
    gw_put_u64(out + 47, incomplete);
    gw_put_u64(out + 55, bad_rx);
    gw_put_u64(out + 63, retries);
    return 71;
}

static uint8_t dispatch_command(uint8_t opcode, const uint8_t *args, size_t length,
                                uint8_t *out, size_t *out_length, uint64_t received_ms)
{
    *out_length = 0;
    if (opcode >= 0x30 && opcode <= 0x37) {
        return gw_dfu_command(opcode, args, length, out, out_length);
    }
    if (gw_dfu_active() && opcode != 0x02 && opcode != 0x03 && opcode != 0x04 &&
        opcode != 0x06 && opcode != 0x08 && opcode != 0x09 && opcode != 0x0a &&
        opcode != 0x0c && opcode != 0x0d && opcode != 0x17) {
        return GW_BUSY;
    }
    switch (opcode) {
    case 0x03:
        if (length) {
            return GW_BAD_PARAM;
        }
        *out_length = encode_status(out);
        return GW_OK;
    case 0x04:
        if (length) {
            return GW_BAD_PARAM;
        }
        gw_put_u64(out, received_ms);
        gw_put_u64(out + 8, k_uptime_get());
        *out_length = 16;
        return GW_OK;
    case 0x0a:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18:
        if (!(gw_radio_capabilities() & BIT(2))) {
            return GW_UNSUPPORTED;
        }
        return gw_gatt_command(opcode, args, length, out, out_length);
    case 0x0d: case 0x0e: case 0x0f:
        if (!(gw_radio_capabilities() & BIT(11))) {
            return GW_UNSUPPORTED;
        }
        return gw_peripheral_command(opcode, args, length, out, out_length);
    default:
        return gw_radio_command(opcode, args, length, out, out_length);
    }
}

static void handle_hello(const struct gw_frame *request)
{
    if (request->payload_len || request->msg_id || !request->link_id) {
        bad_rx++;
        return;
    }
    if (request->link_id != link_id) {
        link_id = request->link_id;
        last_command = 0;
        cached_request_length = 0;
        /* FIFO, frozen batch, config, conn_id and GATT op cache survive relinking. */
    }
    link_established = false;
    outgoing.type = 0x81;
    outgoing.link_id = link_id;
    outgoing.msg_id = 0;
    memcpy(outgoing.payload, boot_id, sizeof(boot_id));
    gw_put_u32(outgoing.payload + 16, gw_radio_capabilities() |
               (gw_dfu_available() ? BIT(12) : 0));
    outgoing.payload[20] = sizeof(FW_VERSION) - 1;
    memcpy(outgoing.payload + 21, FW_VERSION, sizeof(FW_VERSION) - 1);
    outgoing.payload_len = 21 + sizeof(FW_VERSION) - 1;
    if (send_frame(&outgoing) == 0) {
        link_established = true;
        ack_deadline_ms = 0;
        next_batch_send_ms = k_uptime_get();
    }
}

static void handle_command(const struct gw_frame *request, uint64_t received_ms)
{
    if (!request->msg_id || !request->payload_len) {
        bad_rx++;
        return;
    }
    if (request->msg_id < last_command) {
        return;
    }
    if (request->msg_id == last_command) {
        if (request->payload_len == cached_request_length &&
            !memcmp(request->payload, cached_request, cached_request_length)) {
            (void)send_frame(&cached_response);
        } else {
            outgoing.type = 0x90;
            outgoing.link_id = link_id;
            outgoing.msg_id = request->msg_id;
            outgoing.payload_len = 2;
            outgoing.payload[0] = request->payload[0];
            outgoing.payload[1] = GW_CONFLICT;
            (void)send_frame(&outgoing);
        }
        return;
    }
    uint8_t opcode = request->payload[0];
    cached_response.type = 0x90;
    cached_response.link_id = link_id;
    cached_response.msg_id = request->msg_id;
    cached_response.payload[0] = opcode;
    size_t result_length = 0;
    uint8_t status = dispatch_command(opcode, request->payload + 1,
                                     request->payload_len - 1,
                                     cached_response.payload + 2, &result_length,
                                     received_ms);
    cached_response.payload[1] = status;
    cached_response.payload_len = 2 + (status == GW_OK ? result_length : 0);
    last_command = request->msg_id;
    cached_request_length = request->payload_len;
    memcpy(cached_request, request->payload, cached_request_length);
    /* Cache even when TX fails: a host retry must not repeat a physical action. */
    (void)send_frame(&cached_response);
}

static void handle_ack(const struct gw_frame *request)
{
    if (request->payload_len != 3) {
        bad_rx++;
        return;
    }
    uint8_t status = request->payload[0];
    uint16_t interval = gw_get_u16(request->payload + 1);
    if (status > 2 || (status == 1 ? (interval < 250 || interval > 5000) : interval != 0)) {
        bad_rx++;
        return;
    }
    k_mutex_lock(&queue_lock, K_FOREVER);
    bool matched = gw_queue_ack(&queue, request->msg_id, status);
    k_mutex_unlock(&queue_lock);
    if (!matched) {
        return;
    }
    ack_deadline_ms = 0;
    next_batch_send_ms = k_uptime_get() + interval;
    if (status == 0) {
        sent_batch = 0;
        retry_backoff_ms = 250;
    }
}

static void receive_frames(void)
{
    uint8_t bytes[128];
    uint32_t count;
    uint32_t overflow = atomic_set(&rx_overflow, 0);
    uint32_t driver_errors = atomic_set(&rx_driver_errors, 0);
    if (overflow || driver_errors) {
        bad_rx += overflow + driver_errors;
        decoder.length = 0;
        decoder.discard = true;
    }
    /* Bound each turn so notifications, deadlines and driver recovery progress. */
    for (size_t budget = 0; budget < RX_RING_SIZE; budget += count) {
        count = ring_buf_get(&rx_ring, bytes, sizeof(bytes));
        if (!count) {
            break;
        }
        for (size_t i = 0; i < count; ++i) {
            int decoded = gw_decoder_feed(&decoder, bytes[i], &incoming);
            if (decoded < 0) {
                bad_rx++;
            } else if (decoded > 0) {
                uint64_t received_ms = k_uptime_get();
                if (incoming.type == 0x01) {
                    handle_hello(&incoming);
                } else if (link_established && incoming.link_id == link_id) {
                    if (incoming.type == 0x10) {
                        handle_command(&incoming, received_ms);
                    } else if (incoming.type == 0xa0) {
                        handle_ack(&incoming);
                    } else {
                        bad_rx++;
                    }
                }
            }
        }
    }
}

static void transmit_batch(void)
{
    uint64_t now = k_uptime_get();
    if (!link_established || atomic_get(&tx_busy)) {
        return;
    }
    if (ack_deadline_ms) {
        if (now < ack_deadline_ms) {
            return;
        }
        ack_deadline_ms = 0;
        next_batch_send_ms = now + retry_backoff_ms + sys_rand32_get() % 101;
        retry_backoff_ms = MIN(retry_backoff_ms * 2, 5000);
    }
    if (now < next_batch_send_ms) {
        return;
    }
    k_mutex_lock(&queue_lock, K_FOREVER);
    bool ready = gw_queue_batch(&queue, now, &outgoing);
    bool exhausted = !queue.count && (queue.next_sequence == UINT64_MAX || !queue.next_batch);
    k_mutex_unlock(&queue_lock);
    if (exhausted) {
        sys_reboot(SYS_REBOOT_COLD);
        return;
    }
    if (!ready) {
        return;
    }
    outgoing.link_id = link_id;
    if (sent_batch == outgoing.msg_id) {
        retries++;
    }
    sent_batch = outgoing.msg_id;
    if (send_frame(&outgoing) == 0) {
        ack_deadline_ms = k_uptime_get() + 500;
    } else {
        next_batch_send_ms = k_uptime_get() + retry_backoff_ms + sys_rand32_get() % 101;
        retry_backoff_ms = MIN(retry_backoff_ms * 2, 5000);
    }
}

int gw_app_run(void)
{
    gw_queue_init(&queue);
    gw_decoder_init(&decoder);
    int err = gw_dfu_init();
    if (err) {
        LOG_ERR("DFU unavailable (%d); UART diagnostics remain enabled", err);
    }
    /* Never reuse a constant boot identifier if the entropy device fails. */
    err = hold_uart_hfxo();
    if (err) {
        return err;
    }
    err = sys_csrand_get(boot_id, sizeof(boot_id));
    if (err) {
        return err;
    }
    err = gw_radio_init();
    if (err) {
        LOG_ERR("Bluetooth unavailable (%d); UART diagnostics remain enabled", err);
    }
    err = gw_gatt_init(gatt_record);
    if (err) {
        return err;
    }
    err = gw_peripheral_init();
    if (err) {
        return err;
    }
    if (!device_is_ready(link_uart)) {
        return -ENODEV;
    }
    const struct uart_config config = {
        .baudrate = 1000000,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS,
    };
    err = uart_configure(link_uart, &config);
    if (err) {
        return err;
    }
    err = uart_callback_set(link_uart, uart_event, NULL);
    if (err) {
        return err;
    }
    LOG_INF("Gateway %s started (capabilities 0x%x)", FW_VERSION, gw_radio_capabilities());
    restart_rx();
    for (;;) {
        restart_rx();
        receive_frames();
        gw_gatt_poll();
        gw_peripheral_poll();
        gw_dfu_poll();
        gw_radio_poll();
        transmit_batch();
        k_sleep(K_MSEC(1));
    }
}
