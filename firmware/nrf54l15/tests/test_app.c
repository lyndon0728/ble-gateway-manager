/* Exercises real UART/session orchestration with synchronous fake peripherals.
 * The mock cannot validate Nordic DMA ownership races or SDK ABI compatibility. */
#include <assert.h>
#include <stdio.h>
#include "mock_zephyr.h"
#include "../src/gw_app.c"

int64_t mock_now = 1000;
unsigned mock_reboots;
const struct device mock_device = {0};
static mock_uart_callback_t mock_callback;
static struct gw_frame sent[64];
static size_t sent_count;
static unsigned radio_executions;
static bool fail_tx;

int uart_callback_set(const struct device *d,mock_uart_callback_t cb,void *u)
{ ARG_UNUSED(d); ARG_UNUSED(u); mock_callback=cb; return 0; }
int uart_tx(const struct device *d,const uint8_t *wire,size_t n,int t)
{
    ARG_UNUSED(t);
    if (fail_tx) { fail_tx=false; return -EIO; }
    assert(sent_count<64 && n>=2 && wire[0]==0 && wire[n-1]==0);
    assert(gw_decode_frame(wire+1,n-2,&sent[sent_count++]));
    struct uart_event event={.type=UART_TX_DONE};
    mock_callback(d,&event,NULL);
    return 0;
}
int uart_tx_abort(const struct device *d)
{ struct uart_event event={.type=UART_TX_ABORTED}; mock_callback(d,&event,NULL); return 0; }
int gw_radio_init(void) { return 0; }
int gw_gatt_init(void (*cb)(const uint8_t *,size_t)) { ARG_UNUSED(cb); return 0; }
int gw_peripheral_init(void) { return 0; }
int gw_dfu_init(void) { return 0; }
void gw_dfu_poll(void) {}
bool gw_dfu_available(void) { return false; }
bool gw_dfu_active(void) { return false; }
uint8_t gw_dfu_command(uint8_t op,const uint8_t *a,size_t n,uint8_t *o,size_t *l)
{ ARG_UNUSED(op); ARG_UNUSED(a); ARG_UNUSED(n); ARG_UNUSED(o); *l=0; return GW_UNSUPPORTED; }
void gw_peripheral_poll(void) {}
uint8_t gw_peripheral_command(uint8_t op,const uint8_t *a,size_t n,uint8_t *o,size_t *l)
{ return gw_radio_command(op,a,n,o,l); }
void gw_radio_poll(void) {}
void gw_gatt_poll(void) {}
uint32_t gw_radio_capabilities(void) { return BIT(1)|BIT(2); }
uint64_t gw_radio_revision(void) { return 1; }
uint8_t gw_radio_scan_state(void) { return 0; }
void gw_radio_counters(uint64_t *a,uint64_t *b,uint64_t *c) { *a=0; *b=0; *c=0; }
uint8_t gw_radio_command(uint8_t op,const uint8_t *args,size_t n,uint8_t *out,size_t *len)
{ ARG_UNUSED(op); ++radio_executions; out[0]=n?args[0]:0x55; *len=1; return GW_OK; }
uint8_t gw_gatt_command(uint8_t op,const uint8_t *a,size_t n,uint8_t *o,size_t *l)
{ return gw_radio_command(op,a,n,o,l); }

static void reset_test(void)
{
    gw_queue_init(&queue); gw_decoder_init(&decoder); rx_ring.used=0;
    memset(rx_owned,0,sizeof(rx_owned)); rx_disabled=1;
    rx_overflow=rx_driver_errors=tx_busy=tx_success=0;
    link_id=0; link_established=false; last_command=0; cached_request_length=0;
    ack_deadline_ms=next_batch_send_ms=sent_batch=0; retry_backoff_ms=250;
    dropped_full=bad_rx=retries=0; sent_count=radio_executions=mock_reboots=0;
    fail_tx=false; mock_now=1000; mock_callback=uart_event;
    memset(boot_id,0x42,sizeof(boot_id));
}
static void feed(const struct gw_frame *f)
{
    uint8_t wire[GW_WIRE_MAX]; size_t n=gw_encode_frame(f,wire,sizeof(wire));
    assert(n>0);
    /* Split frame across UART callbacks; exercise actual incremental parser. */
    for (size_t pos=0;pos<n;) {
        size_t chunk=MIN((size_t)7,n-pos);
        struct uart_event event={.type=UART_RX_RDY,.data.rx={wire,pos,chunk}};
        uart_event(&mock_device,&event,NULL); receive_frames(); pos+=chunk;
    }
}
static void hello(uint64_t id)
{ struct gw_frame f={.type=0x01,.link_id=id}; feed(&f); }
static struct gw_frame command(uint32_t id,uint8_t arg)
{ struct gw_frame f={.type=0x10,.link_id=link_id,.msg_id=id,.payload_len=2,.payload={1,arg}}; return f; }
static void ack(uint64_t session,uint32_t id,uint8_t status,uint16_t delay)
{
    struct gw_frame f={.type=0xa0,.link_id=session,.msg_id=id,.payload_len=3};
    f.payload[0]=status; gw_put_u16(f.payload+1,delay); feed(&f);
}
static void enqueue(uint8_t marker)
{ uint8_t record[24]={0}; record[0]=marker; assert(gw_app_record(0x20,record,sizeof(record))); }

static void test_handshake_and_session(void)
{
    reset_test(); hello(0); assert(!link_established && sent_count==0 && bad_rx==1);
    hello(0x1234); assert(link_established && sent_count==1 && link_id==0x1234);
    assert(sent[0].type==0x81 && sent[0].msg_id==0 && sent[0].link_id==link_id);
    assert(!memcmp(sent[0].payload,boot_id,16));
    assert(gw_get_u32(sent[0].payload+16)==gw_radio_capabilities());
    struct gw_frame f=command(1,7); f.link_id=0x9999; feed(&f);
    assert(radio_executions==0 && sent_count==1);
}
static void test_command_idempotence_conflict(void)
{
    reset_test(); hello(10); struct gw_frame f=command(1,0x33); feed(&f);
    struct gw_frame first=sent[1]; feed(&f);
    assert(radio_executions==1 && sent_count==3);
    assert(sent[2].payload_len==first.payload_len && !memcmp(sent[2].payload,first.payload,first.payload_len));
    f.payload[1]=0x44; feed(&f); assert(radio_executions==1);
    assert(sent[3].payload[1]==GW_CONFLICT && sent[3].payload_len==2);
    f.payload[1]=0x33; feed(&f); assert(sent[4].payload[1]==GW_OK && radio_executions==1);
    f.msg_id=2; feed(&f); assert(radio_executions==2);
    size_t count=sent_count; f.msg_id=1; feed(&f); assert(sent_count==count && radio_executions==2);
}
static void test_failed_response_cached(void)
{
    reset_test(); hello(11); struct gw_frame f=command(1,9); fail_tx=true; feed(&f);
    assert(radio_executions==1 && sent_count==1);
    feed(&f); assert(radio_executions==1 && sent_count==2 && sent[1].payload[1]==GW_OK);
}
static void test_relink_preserves_fifo(void)
{
    reset_test(); hello(12); enqueue(0x31); mock_now+=60; transmit_batch();
    assert(queue.inflight_count==1); struct gw_frame frozen=sent[sent_count-1];
    hello(13); assert(queue.count==1 && queue.inflight_count==1 && last_command==0);
    transmit_batch(); struct gw_frame resent=sent[sent_count-1];
    assert(resent.type==0x20 && resent.link_id==13 && resent.msg_id==frozen.msg_id);
    assert(resent.payload_len==frozen.payload_len && !memcmp(resent.payload,frozen.payload,frozen.payload_len));
    ack(12,frozen.msg_id,0,0); assert(queue.count==1);
    ack(13,frozen.msg_id+99,0,0); assert(queue.count==1);
    ack(13,frozen.msg_id,0,0); assert(queue.count==0 && queue.used_bytes==0);
}
static void test_busy_freezes_batch_and_custody(void)
{
    reset_test(); hello(14); enqueue(1); mock_now+=60; transmit_batch();
    struct gw_frame frozen=sent[sent_count-1]; enqueue(2);
    ack(14,frozen.msg_id,1,300); assert(queue.count==2 && queue.inflight_count==1);
    size_t before=sent_count; mock_now+=299; transmit_batch(); assert(sent_count==before);
    ++mock_now; transmit_batch(); assert(sent_count==before+1);
    assert(sent[before].msg_id==frozen.msg_id && sent[before].payload_len==frozen.payload_len);
    assert(!memcmp(sent[before].payload,frozen.payload,frozen.payload_len));
    ack(14,frozen.msg_id,0,0); assert(queue.count==1 && queue.used_bytes==24 && queue.inflight_count==0);
    transmit_batch(); assert(sent[sent_count-1].msg_id==frozen.msg_id+1);
    ack(14,frozen.msg_id+1,0,0); assert(queue.count==0 && queue.used_bytes==0);
}
static void test_invalid_ack_and_fault(void)
{
    reset_test(); hello(15); enqueue(1); mock_now+=60; transmit_batch(); uint32_t id=queue.inflight_batch;
    ack(15,id,1,249); assert(bad_rx==1 && queue.inflight_count==1);
    ack(15,id,0,1); assert(bad_rx==2 && queue.count==1);
    ack(15,id,2,0); assert(queue.fault && queue.count==1);
    size_t before=sent_count; mock_now+=10000; transmit_batch(); assert(sent_count==before);
}
int main(void)
{
    test_handshake_and_session(); test_command_idempotence_conflict();
    test_failed_response_cached(); test_relink_preserves_fifo();
    test_busy_freezes_batch_and_custody(); test_invalid_ack_and_fault();
    puts("PASS: 6 app transport scenarios (real gw_app + gw_core; mock peripherals)");
    return 0;
}
