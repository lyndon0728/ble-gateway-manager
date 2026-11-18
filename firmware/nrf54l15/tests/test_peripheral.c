/* Application lifecycle tests with controlled stack callbacks, not an SDK ABI
 * or over-the-air test. Build with -Itests/peripheral_mock. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../src/gw_peripheral.c"

static bool queue_accept;
static uint8_t queued[536],out[16];
static size_t queued_len,out_len;
static unsigned int queue_calls,radio_changes,radio_connections;
bool gw_app_record(uint8_t type,const uint8_t *record,size_t len)
{
    assert(mock_lock_depth==0 && type==0x22 && len<=sizeof(queued));
    ++queue_calls;
    if (!queue_accept) { return false; }
    memcpy(queued,record,len); queued_len=len; return true;
}
void gw_radio_peripheral_changed(void) { assert(mock_lock_depth==0); ++radio_changes; }
void gw_radio_peripheral_connected(void) { assert(mock_lock_depth==0); ++radio_connections; }

static void reset(void)
{
    memset(&peripheral,0,sizeof(peripheral));
    mock_conn=(struct bt_conn){.refs=1,.role=BT_CONN_ROLE_PERIPHERAL,.security=1,.mtu=247};
    mock_peer=(bt_addr_le_t){.type=1,.a.val={1,2,3,4,5,6}};
    mock_time=0; mock_lock_depth=0; mock_disconnects=0; mock_disconnect_error=0;
    mock_subscribed=false; mock_complete_immediately=false; mock_notify_error=0;
    mock_notifies=0; mock_last_notify=NULL; mock_notify_hook=NULL;
    queue_accept=true; queued_len=0; queue_calls=0; radio_changes=0; radio_connections=0;
    assert(gw_peripheral_init()==0);
}
static void connect(void)
{
    mock_conn.refs=1; peripheral_connected(&mock_conn,0);
    assert(peripheral.state==P_CONNECTED && gw_peripheral_busy() && mock_conn.refs==2);
}
static void disconnect(void)
{
    peripheral_disconnected(&mock_conn,0);
    assert(gw_peripheral_busy() && peripheral.draining);
    bt_conn_unref(&mock_conn);
    assert(!gw_peripheral_busy());
}
static void tx_request(uint8_t *p,uint64_t seq,uint32_t id)
{
    sys_put_le64(seq,p); sys_put_le32(id,p+8); sys_put_le16(3,p+12);
    p[14]=0x10; p[15]=0x20; p[16]=0x30;
}

static void test_service_and_public_info(void)
{
    reset(); uint8_t info[13]; mock_time=123456;
    assert(sizeof(peripheral_service_attrs)/sizeof(peripheral_service_attrs[0])==8);
    assert(peripheral_service.attrs[1].properties==BT_GATT_CHRC_READ);
    assert(peripheral_service.attrs[2].perm==BT_GATT_PERM_READ);
    assert(peripheral_service.attrs[3].properties==BT_GATT_CHRC_WRITE);
    assert(peripheral_service.attrs[4].perm==BT_GATT_PERM_WRITE);
    assert(peripheral_service.attrs[5].properties==BT_GATT_CHRC_NOTIFY);
    const uint8_t expected[16]={0,0x54,0x0b,0x9a,0x2f,0x6c,0x91,0x9d,0x31,0x4c,0x5a,0x4b,1,0,0xe0,0x7b};
    assert(!memcmp(service_uuid.val,expected,16));
    assert(info_read(&mock_conn,&peripheral_service.attrs[2],info,sizeof(info),0)==13);
    assert(info[0]==1 && sys_get_le32(info+1)==7 && sys_get_le64(info+5)==123456);
    assert(info_read(&mock_conn,&peripheral_service.attrs[2],info,sizeof(info),14)==-BT_ATT_ERR_INVALID_OFFSET);
}
static void test_role_and_state(void)
{
    reset(); mock_conn.role=BT_CONN_ROLE_CENTRAL;
    peripheral_connected(&mock_conn,0); assert(!gw_peripheral_busy() && mock_conn.refs==1);
    assert(gw_peripheral_command(0x0D,NULL,0,out,&out_len)==P_OK && out_len==16);
    for (size_t i=0;i<16;i++) { assert(out[i]==0); }
    mock_conn.role=BT_CONN_ROLE_PERIPHERAL; connect(); mock_subscribed=true; mock_conn.security=3;
    assert(gw_peripheral_command(0x0D,NULL,0,out,&out_len)==P_OK);
    assert(sys_get_le32(out)==1 && out[4]==1 && out[5]==1 && out[6]==6 && out[11]==1);
    assert(sys_get_le16(out+12)==247 && out[14]==1 && out[15]==2);
    disconnect(); connect(); assert(peripheral.conn_id==2);
}
static void test_rx_queue_ack_and_validation(void)
{
    reset(); connect(); uint8_t data[245]={0xaa,0xbb,0xcc}; mock_time=77;
    const struct bt_gatt_attr *attr=&peripheral_service.attrs[4];
    assert(rx_write(&mock_conn,attr,data,3,0,0)==3 && queued_len==27);
    assert(sys_get_le64(queued)==77 && sys_get_le32(queued+8)==1);
    assert(queued[12]==1 && queued[13]==6 && sys_get_le16(queued+19)==1 && queued[21]==0);
    assert(sys_get_le16(queued+22)==3 && !memcmp(queued+24,data,3));
    queue_accept=false;
    assert(rx_write(&mock_conn,attr,data,3,0,0)==-BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    unsigned int calls=queue_calls;
    assert(rx_write(&mock_conn,attr,data,3,0,BT_GATT_WRITE_FLAG_CMD)==-BT_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(rx_write(&mock_conn,attr,data,3,0,BT_GATT_WRITE_FLAG_PREPARE)==-BT_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(rx_write(&mock_conn,attr,data,3,0,BT_GATT_WRITE_FLAG_EXECUTE)==-BT_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(rx_write(&mock_conn,attr,data,3,1,0)==-BT_ATT_ERR_INVALID_OFFSET);
    assert(rx_write(&mock_conn,attr,data,0,0,0)==-BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    assert(rx_write(&mock_conn,attr,data,245,0,0)==-BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    assert(queue_calls==calls);
}
static void test_notify_replay_and_ownership(void)
{
    reset(); connect(); uint8_t p[17]; tx_request(p,1,1);
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_CONFLICT);
    mock_subscribed=true;
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK && out_len==8);
    assert(mock_notifies==1 && peripheral.tx.pending && sys_get_le64(out)==1);
    assert(mock_last_notify->data!=p+14 && mock_last_notify->attr==&peripheral_service.attrs[6]);
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK && mock_notifies==1);
    p[14]++; assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_CONFLICT);
    assert(((const uint8_t *)mock_last_notify->data)[0]==0x10); p[14]--;
    sys_put_le64(2,p); assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_BUSY);
    mock_last_notify->func(&mock_conn,mock_last_notify->user_data);
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK && mock_notifies==2);
    sys_put_le64(1,p); assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_STALE);
    sys_put_le64(2,p); disconnect();
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK && mock_notifies==2);
    sys_put_le64(3,p); assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_NOT_FOUND);
}
static void test_notify_failure_and_sync_completion(void)
{
    reset(); connect(); mock_subscribed=true; uint8_t p[17]; tx_request(p,1,1);
    mock_notify_error=-ENOMEM;
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_BUSY);
    assert(peripheral.accepted.seq==0 && !peripheral.tx.pending);
    mock_notify_error=0; mock_complete_immediately=true;
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK);
    assert(peripheral.accepted.seq==1 && !peripheral.tx.pending && !peripheral.tx.launching);
}
static void disconnect_during_notify(struct bt_conn *conn)
{
    peripheral_disconnected(conn,0); bt_conn_unref(conn);
    assert(peripheral.draining); /* Command's temporary reference still owns it. */
}
static void test_disconnect_during_acceptance(void)
{
    reset(); connect(); mock_subscribed=true; uint8_t p[17]; tx_request(p,1,1);
    mock_notify_hook=disconnect_during_notify;
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK);
    assert(!gw_peripheral_busy() && peripheral.accepted.seq==1 && !peripheral.tx.pending);
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK && mock_notifies==1);
}
static void test_disconnect_and_watchdog(void)
{
    reset(); connect(); uint8_t id[4]; sys_put_le32(1,id);
    assert(gw_peripheral_command(0x0F,id,4,out,&out_len)==P_OK && !out_len);
    gw_peripheral_poll(); gw_peripheral_poll();
    assert(mock_disconnects==1 && peripheral.state==P_CLOSING);
    disconnect(); assert(gw_peripheral_command(0x0F,id,4,out,&out_len)==P_NOT_FOUND);
    reset(); connect(); mock_subscribed=true; uint8_t p[17]; tx_request(p,1,1);
    assert(gw_peripheral_command(0x0E,p,sizeof(p),out,&out_len)==P_OK);
    mock_time=P_TX_DRAIN_MS+1; gw_peripheral_poll();
    assert(peripheral.state==P_CLOSING && mock_disconnects==1 && peripheral.accepted.seq==1);
    disconnect(); assert(!peripheral.tx.pending && peripheral.accepted.seq==1);
}
static void test_fast_cycle_and_admission_gate(void)
{
    reset(); connect(); disconnect();
    /* No peripheral/radio poll ran between connect and full recycle. */
    assert(radio_connections==1 && radio_changes==2 && !gw_peripheral_busy());
    reset(); gw_peripheral_set_admission(false);
    peripheral_connected(&mock_conn,0);
    assert(peripheral.state==P_CLOSING && gw_peripheral_busy() && radio_connections==1);
    uint8_t data[17]; tx_request(data,1,1); mock_subscribed=true;
    assert(gw_peripheral_command(0x0E,data,sizeof(data),out,&out_len)==P_BUSY);
    assert(rx_write(&mock_conn,&peripheral_service.attrs[4],data,3,0,0)==-BT_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(queue_calls==0 && mock_notifies==0);
    gw_peripheral_set_admission(true);
    assert(peripheral.state==P_CLOSING); /* Restoring admission cannot revive a late link. */
    gw_peripheral_poll(); assert(mock_disconnects==1);
    disconnect(); connect(); assert(peripheral.conn_id==2 && peripheral.state==P_CONNECTED);
    gw_peripheral_set_admission(false); assert(peripheral.state==P_CLOSING);
}
int main(void)
{
    test_service_and_public_info(); test_role_and_state(); test_rx_queue_ack_and_validation();
    test_notify_replay_and_ownership(); test_notify_failure_and_sync_completion();
    test_disconnect_during_acceptance(); test_disconnect_and_watchdog();
    test_fast_cycle_and_admission_gate();
    assert(mock_lock_depth==0); puts("Peripheral lifecycle: 8 scenarios passed"); return 0;
}
