/* Deterministic failure injection against the production executor. Mocks only
 * replace IDF/UART/Flash/clock boundaries; operation/state code is unchanged. */
#include <assert.h>
#include "../gw_gatt_nrf.c"

static cJSON *db;
static uint8_t peer_boot[16]={1},last_request[530];
static uint8_t last_opcode;
static uint64_t ticks;
static unsigned sends,writes,link_events,atomic_connection_results;
static bool fail_store,lost_accept,unknown_result,corrupt_result,connected,bad_readback;

uint16_t gw_get_u16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
uint32_t gw_get_u32(const uint8_t *p){return (uint32_t)gw_get_u16(p)|(uint32_t)gw_get_u16(p+2)<<16;}
uint64_t gw_get_u64(const uint8_t *p){return (uint64_t)gw_get_u32(p)|(uint64_t)gw_get_u32(p+4)<<32;}
void gw_put_u16(uint8_t *p,uint16_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
void gw_put_u32(uint8_t *p,uint32_t x){gw_put_u16(p,(uint16_t)x);gw_put_u16(p+2,(uint16_t)(x>>16));}
void gw_put_u64(uint8_t *p,uint64_t x){gw_put_u32(p,(uint32_t)x);gw_put_u32(p+4,(uint32_t)(x>>32));}
uint64_t gw_core_mono_ms(void){return ticks;}
esp_err_t gw_core_maintenance_acquire(const char *s){(void)s;return ESP_OK;}
void gw_core_maintenance_release(const char *s){(void)s;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void *)1;}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t t){(void)s;(void)t;return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t s){(void)s;}
void vTaskDelay(TickType_t t){ticks+=t;}
int xTaskCreate(void (*f)(void *),const char *n,unsigned stack,void *p,unsigned priority,void *handle){(void)f;(void)n;(void)stack;(void)p;(void)priority;(void)handle;return pdPASS;}
cJSON *gw_store_get_json(const char *key){return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(db,key),true);}
esp_err_t gw_store_put_json(const char *key,const cJSON *o){if(fail_store)return ESP_FAIL;cJSON_DeleteItemFromObjectCaseSensitive(db,key);cJSON_AddItemToObject(db,key,cJSON_Duplicate(o,true));return ESP_OK;}
esp_err_t gw_store_get(const char *key,void *out,size_t *n){(void)out;(void)n;return cJSON_GetObjectItemCaseSensitive(db,key)?ESP_OK:ESP_ERR_NOT_FOUND;}
esp_err_t gw_store_transaction(const gw_store_op_t *ops,size_t count){if(fail_store)return ESP_FAIL;assert(count==1||count==2);if(count==2)atomic_connection_results++;for(size_t i=0;i<count;i++){char *raw=malloc(ops[i].size+1);assert(raw);memcpy(raw,ops[i].data,ops[i].size);raw[ops[i].size]=0;cJSON *v=cJSON_Parse(raw);assert(v);assert(gw_store_put_json(ops[i].key,v)==ESP_OK);free(raw);cJSON_Delete(v);}return ESP_OK;}
bool gw_nrf_identity(uint8_t boot[16],uint32_t *caps,char version[64]){if(boot)memcpy(boot,peer_boot,16);if(caps)*caps=(1u<<2)|(1u<<3)|(1u<<9);if(version)strcpy(version,"test");return true;}
void gw_capture_connection_ref(uint8_t radio,const uint8_t boot[16],uint32_t id,char out[33]){(void)radio;(void)boot;(void)id;strcpy(out,"01234567890123456789012345678901");}
bool gw_capture_adv_evidence(const char *event,const char *addr,const char *type,const uint8_t boot[16],uint32_t age){(void)addr;(void)type;(void)boot;(void)age;return !strcmp(event,"fresh");}
esp_err_t gw_capture_link_down(uint8_t radio,const uint8_t boot[16],uint32_t id,uint8_t type,const uint8_t addr[6],const char *reason){(void)radio;(void)boot;(void)id;(void)type;(void)addr;(void)reason;link_events++;return ESP_OK;}

/* Test-only codec keeps these tests independent of a crypto installation. */
int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *n,const unsigned char *in,size_t size){static const char b[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";size_t need=((size+2)/3)*4;if(cap<need+1)return -1;size_t j=0;for(size_t i=0;i<size;i+=3){uint32_t v=(uint32_t)in[i]<<16;if(i+1<size)v|=(uint32_t)in[i+1]<<8;if(i+2<size)v|=in[i+2];out[j++]=b[v>>18];out[j++]=b[(v>>12)&63];out[j++]=i+1<size?b[(v>>6)&63]:'=';out[j++]=i+2<size?b[v&63]:'=';}out[j]=0;*n=j;return 0;}
int mbedtls_base64_decode(unsigned char *out,size_t cap,size_t *n,const unsigned char *in,size_t size){static const char b[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";if(size%4)return -1;size_t j=0;for(size_t i=0;i<size;i+=4){uint32_t v=0;for(unsigned k=0;k<4;k++){const char *p=strchr(b,in[i+k]);if(!p&&in[i+k]!='=')return -1;v=(v<<6)|(p?(unsigned)(p-b):0);}if(j>=cap)return -1;out[j++]=(uint8_t)(v>>16);if(in[i+2]!='='){if(j>=cap)return -1;out[j++]=(uint8_t)(v>>8);}if(in[i+3]!='='){if(j>=cap)return -1;out[j++]=(uint8_t)v;}}*n=j;return 0;}

esp_err_t gw_nrf_command(uint8_t opcode,const void *data,size_t size,uint8_t *status,void *output,size_t *out_size,int64_t deadline){
    (void)deadline;uint8_t *out=output;*status=0;
    if(opcode>=0x10&&opcode<=0x18&&opcode!=0x17){
        const uint8_t *request=data;cJSON *j=gw_store_get_json(OP_KEY);assert(j);assert(!strcmp(str(j,"phase"),"INTENT"));uint64_t seq=0;assert(decimal(str(j,"sequence"),&seq));assert(seq==gw_get_u64(request));char encoded[1061];hex(request,size,encoded);assert(!strcmp(encoded,str(j,"argsHex")));cJSON_Delete(j);
        sends++;if(opcode==0x14)writes++;memcpy(last_request,data,size);last_opcode=opcode;
        if(lost_accept){*status=255;return ESP_ERR_TIMEOUT;}assert(*out_size>=8);memcpy(out,request,8);*out_size=8;return ESP_OK;
    }
    if(opcode==0x17){
        assert(*out_size>=528);memset(out,0,12);memcpy(out,last_request,8);out[8]=corrupt_result?0xff:last_opcode;out[9]=unknown_result?4:1;*out_size=12;
        if(unknown_result)return ESP_OK;
        if(last_opcode==0x10){connected=true;gw_put_u32(out+12,42);gw_put_u16(out+16,23);*out_size=18;}
        if(last_opcode==0x11)connected=false;
        if(last_opcode==0x12){gw_put_u16(out+12,7);gw_put_u16(out+14,8);out[16]=0x3a;*out_size=17;}
        if(last_opcode==0x13){gw_put_u16(out+12,3);memcpy(out+14,bad_readback?"bad":"abc",3);*out_size=17;}
        if(last_opcode==0x18){gw_put_u16(out+12,1);gw_put_u16(out+14,9);*out_size=16;}return ESP_OK;
    }
    if(opcode==0x0a){if(!connected){*status=7;*out_size=0;return ESP_OK;}assert(*out_size>=14);gw_put_u32(out,42);out[4]=0;const uint8_t addr[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff};memcpy(out+5,addr,6);gw_put_u16(out+11,23);out[13]=0;*out_size=14;return ESP_OK;}
    assert(!"unexpected command");return ESP_FAIL;
}
static void reset(void){cJSON_Delete(db);db=cJSON_CreateObject();memset(peer_boot,0,16);peer_boot[0]=1;memset(last_request,0,sizeof(last_request));sends=writes=link_events=atomic_connection_results=0;ticks=100;fail_store=lost_accept=unknown_result=corrupt_result=connected=bad_readback=false;storage_fault=false;recovering=false;lock=(void *)1;}
static cJSON *command(const char *type){cJSON *c=cJSON_CreateObject();string(c,"taskId","task-1");string(c,"type",type);cJSON *p=cJSON_AddObjectToObject(c,"parameters");string(p,"radio","NRF54");string(p,"deviceAddress","AA:BB:CC:DD:EE:FF");string(p,"addressType","PUBLIC");string(p,"security","UNENCRYPTED");number(p,"timeoutMs",10000);string(p,"serviceUuid","180f");string(p,"characteristicUuid","2a19");string(p,"writeMode","WITH_RESPONSE");string(p,"verification","READBACK");string(p,"valueBase64","YWJj");return c;}
int main(void){
    uint8_t a[8]={0},out[516],u[16];char canonical[37];size_t n=sizeof(out);
    assert(uuid("180F",u,canonical)&&!strcmp(canonical,"0000180f-0000-1000-8000-00805f9b34fb"));assert(!uuid("0000180f0000-1000-8000-00805f9b34fb-",u,canonical));assert(!address("AA-BB-CC-DD-EE-FF",a));
    reset();assert(operation("one",peer_boot,0x10,a,8,10000,out,&n)==ESP_OK);assert(sends==1);n=sizeof(out);assert(operation("two",peer_boot,0x11,a,4,10000,out,&n)==ESP_OK);assert(gw_get_u64(last_request)==2);
    reset();fail_store=true;n=sizeof(out);assert(operation("one",peer_boot,0x14,a,8,10000,out,&n)!=ESP_OK&&sends==0);
    reset();lost_accept=true;n=sizeof(out);assert(operation("one",peer_boot,0x14,a,8,10000,out,&n)==ESP_ERR_TIMEOUT&&writes==1);lost_accept=false;unknown_result=true;n=sizeof(out);assert(operation("two",peer_boot,0x14,a,8,10000,out,&n)==ESP_ERR_TIMEOUT&&writes==1);
    reset();corrupt_result=true;n=sizeof(out);assert(operation("one",peer_boot,0x10,a,8,10000,out,&n)==ESP_ERR_TIMEOUT);assert(!strcmp(str(cJSON_GetObjectItemCaseSensitive(db,OP_KEY),"phase"),"UNKNOWN"));
    reset();cJSON *c=command("GATT_WRITE"),*d=NULL;assert(gw_gatt_nrf_execute(c,10100,&d)==ESP_OK);assert(writes==1&&sends==5&&link_events==1);assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d,"attResponse")));assert(!strcmp(str(d,"readbackBase64"),"YWJj")&&!strcmp(str(d,"connectionState"),"CLOSED"));cJSON_Delete(d);d=NULL;assert(gw_gatt_nrf_recover(c,10100,&d)==ESP_OK&&writes==1);cJSON_Delete(d);cJSON_Delete(c);
    reset();bad_readback=true;c=command("GATT_WRITE");d=NULL;assert(gw_gatt_nrf_execute(c,10100,&d)!=ESP_OK&&writes==1&&!connected);cJSON_Delete(d);cJSON_Delete(c);
    reset();c=command("GATT_CONNECT");cJSON *automatic=cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(c,"parameters"),"autoTrigger");string(automatic,"advEventId","stale");number(automatic,"maxAgeMs",5000);d=NULL;assert(gw_gatt_nrf_execute(c,10100,&d)!=ESP_OK&&sends==0&&!strcmp(str(d,"code"),"STALE_OBSERVATION"));cJSON_Delete(d);cJSON_Delete(c);
    reset();c=command("GATT_CONNECT");d=NULL;assert(gw_gatt_nrf_execute(c,10100,&d)==ESP_OK&&atomic_connection_results==1);assert(!strcmp(str(cJSON_GetObjectItemCaseSensitive(db,CONN_KEY),"code"),"OK"));cJSON_Delete(d);cJSON_Delete(c);cJSON_Delete(db);puts("PASS: nRF GATT durable intent/sequence, unknown write guard, strict identifiers, READBACK/cleanup, freshness and atomic connection/result");return 0;
}
