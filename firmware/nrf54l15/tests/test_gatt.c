/* Deterministic application lifecycle tests. These do not emulate the radio or
 * prove Zephyr/NCS ABI compatibility. Build with -Itests/gatt_mock. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../src/gw_gatt.c"

static uint8_t output[526], record_copy[536];
static size_t output_len, record_len;
int gw_radio_prepare_connect(void) { assert(mock_lock_depth == 0); return 0; }
void gw_radio_connect_complete(void) { assert(mock_lock_depth == 0); }
static void record(const uint8_t *p,size_t n) { memcpy(record_copy,p,n); record_len=n; }
static void reset(void)
{
    memset(&g,0,sizeof(g)); memset(&mock_conn,0,sizeof(mock_conn));
    mock_time=0; mock_lock_depth=0; mock_disconnects=0; mock_writes=0;
    mock_subscribes=0; mock_unsubscribes=0; record_len=0;
    assert(gw_gatt_init(record)==0);
}
static void request(uint8_t *p,size_t n,uint64_t seq)
{
    memset(p,0,n); sys_put_le64(seq,p); sys_put_le16(100,p+8);
    if (n >= 14) { sys_put_le32(1,p+10); }
}
static void establish(void)
{
    uint8_t p[18]; request(p,sizeof(p),1); p[10]=0;
    assert(gw_gatt_command(0x10,p,sizeof(p),output,&output_len)==OK);
    gw_gatt_poll(); assert(g.conn_state==INITIATING);
    connected_cb(&mock_conn,0); gw_gatt_poll();
    assert(g.op.phase==MTU_WAIT); mtu_cb(&mock_conn,0,&g.mtu);
    assert(g.op.state==SUCCEEDED && g.conn_id==1);
    g.handles[0]=(struct handle_entry){42,43,0x3a};
}
static void test_replay_and_write_deadline(void)
{
    reset(); establish();
    uint8_t p[19]; request(p,sizeof(p),2); sys_put_le16(42,p+14); sys_put_le16(1,p+16); p[18]=7;
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK);
    gw_gatt_poll(); assert(mock_writes==1);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK);
    p[18]=8; assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==CONFLICT);
    p[18]=7; mock_time=101; gw_gatt_poll();
    assert(g.op.state==UNKNOWN && g.op.error==E_DEADLINE && mock_disconnects==1);
    write_cb(&mock_conn,0,&g.write); assert(g.op.state==UNKNOWN);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK && mock_writes==1);
    sys_put_le64(3,p); assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==BUSY);
    disconnected_cb(&mock_conn,0); assert(!g.draining && !g.conn);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==NOT_FOUND);
}
static void test_callback_deadline_and_host_error(void)
{
    reset(); establish();
    uint8_t p[19]; request(p,sizeof(p),2); sys_put_le16(42,p+14); sys_put_le16(1,p+16);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    mock_time=101; write_cb(&mock_conn,0,&g.write);
    assert(g.op.state==UNKNOWN && g.close_needed);
    reset(); establish(); request(p,sizeof(p),2); sys_put_le16(42,p+14); sys_put_le16(1,p+16);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    write_cb(&mock_conn,BT_ATT_ERR_UNLIKELY,&g.write);
    assert(g.op.state==UNKNOWN && g.op.att_error==0);
    reset(); establish(); request(p,sizeof(p),2); sys_put_le16(42,p+14); sys_put_le16(1,p+16);
    assert(gw_gatt_command(0x14,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    disconnected_cb(&mock_conn,0);
    assert(g.op.state==UNKNOWN && g.op.error==E_LINK && g.op.att_error==0);
}
static void test_late_connection(void)
{
    reset(); uint8_t p[18]; request(p,sizeof(p),1); p[10]=0;
    assert(gw_gatt_command(0x10,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    mock_time=101; gw_gatt_poll(); assert(g.op.state==TIMEOUT);
    connected_cb(&mock_conn,0); assert(g.close_needed);
    gw_gatt_poll(); assert(mock_disconnects==2 && g.conn_state==CLOSING);
    disconnected_cb(&mock_conn,0); assert(!g.conn && !g.draining);
}
static void test_recycle_barrier(void)
{
    reset(); establish(); bt_conn_ref(&mock_conn);
    disconnected_cb(&mock_conn,0); assert(g.draining && !g.conn);
    uint8_t p[18]; request(p,sizeof(p),2); p[10]=0;
    assert(gw_gatt_command(0x10,p,sizeof(p),output,&output_len)==BUSY);
    bt_conn_unref(&mock_conn); assert(!g.draining);
    assert(gw_gatt_command(0x10,p,sizeof(p),output,&output_len)==OK);
    gw_gatt_poll(); assert(g.conn_id==2);
}
static void test_read_limit(void)
{
    reset(); establish(); uint8_t p[16], data[256]={0};
    request(p,sizeof(p),2); sys_put_le16(42,p+14);
    assert(gw_gatt_command(0x13,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    read_cb(&mock_conn,0,&g.read,data,256); read_cb(&mock_conn,0,&g.read,data,256);
    read_cb(&mock_conn,0,&g.read,NULL,0);
    assert(g.op.state==SUCCEEDED && g.op.result_len==514 && sys_get_le16(g.op.result)==512);
    sys_put_le64(3,p); assert(gw_gatt_command(0x13,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    read_cb(&mock_conn,0,&g.read,data,256); read_cb(&mock_conn,0,&g.read,data,256);
    assert(read_cb(&mock_conn,0,&g.read,data,1)==BT_GATT_ITER_STOP);
    assert(g.op.state==FAILED && g.op.error==E_LENGTH);
}
static void test_subscription_and_record(void)
{
    reset(); establish(); uint8_t p[19]; request(p,sizeof(p),2);
    sys_put_le16(42,p+14); sys_put_le16(43,p+16); p[18]=1;
    assert(gw_gatt_command(0x15,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    struct bt_gatt_subscribe_params *s=&g.subscriptions[0].params;
    subscribe_cb(&mock_conn,0,s); assert(g.op.state==SUCCEEDED);
    uint8_t data[]={9,8}; notify_cb(&mock_conn,s,data,sizeof(data));
    assert(record_len==26 && sys_get_le32(record_copy+8)==1 && record_copy[24]==9);
    notify_cb(&mock_conn,s,data,0); assert(record_len==24 && g.subscriptions[0].active);
    request(p,18,3); sys_put_le16(42,p+14); sys_put_le16(43,p+16);
    assert(gw_gatt_command(0x16,p,18,output,&output_len)==OK); gw_gatt_poll();
    notify_cb(&mock_conn,s,NULL,0); assert(g.op.state==RUNNING);
    subscribe_cb(&mock_conn,0,s); assert(g.op.state==SUCCEEDED && mock_unsubscribes==1);
}
static void test_descriptor_bound_and_ambiguity(void)
{
    reset(); establish(); uint8_t p[46]; request(p,sizeof(p),2); p[29]=1; p[45]=2;
    assert(gw_gatt_command(0x12,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    struct bt_gatt_service_val service={.end_handle=80};
    struct bt_gatt_attr attr={.handle=10,.user_data=&service};
    discover_cb(&mock_conn,&attr,&g.discover); discover_cb(&mock_conn,NULL,&g.discover); gw_gatt_poll();
    struct bt_gatt_chrc ch={.uuid=&g.char_uuid.uuid,.value_handle=20,.properties=0x1a};
    attr=(struct bt_gatt_attr){.handle=19,.user_data=&ch}; discover_cb(&mock_conn,&attr,&g.discover);
    struct bt_uuid_128 other={.uuid.type=BT_UUID_TYPE_128};
    ch.uuid=&other.uuid; ch.value_handle=31; attr.handle=30; discover_cb(&mock_conn,&attr,&g.discover);
    discover_cb(&mock_conn,NULL,&g.discover); gw_gatt_poll();
    assert(g.discover.start_handle==21 && g.discover.end_handle==29);
    attr.handle=21; attr.user_data=NULL; discover_cb(&mock_conn,&attr,&g.discover);
    discover_cb(&mock_conn,NULL,&g.discover);
    assert(g.op.state==SUCCEEDED && sys_get_le16(g.op.result+2)==21);
    request(p,sizeof(p),3); assert(gw_gatt_command(0x12,p,sizeof(p),output,&output_len)==OK); gw_gatt_poll();
    attr.user_data=&service; attr.handle=10; discover_cb(&mock_conn,&attr,&g.discover);
    attr.handle=90; service.end_handle=100; discover_cb(&mock_conn,&attr,&g.discover);
    discover_cb(&mock_conn,NULL,&g.discover); assert(g.op.state==FAILED && g.op.error==E_AMBIGUOUS);
}
int main(void)
{
    test_replay_and_write_deadline(); test_callback_deadline_and_host_error();
    test_late_connection(); test_recycle_barrier(); test_read_limit();
    test_subscription_and_record(); test_descriptor_bound_and_ambiguity();
    assert(mock_lock_depth==0); puts("GATT lifecycle: 7 scenarios passed"); return 0;
}
