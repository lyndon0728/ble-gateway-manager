/* Production state machine with deterministic I/O boundaries. Hash callbacks
 * below are test identities only: these tests do not assess cryptography. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define portMAX_DELAY (~0u)
#include "../gw_core.c"
#include "../gw_validate.c"

static cJSON *disk;
static cJSON *applied;
static uint64_t now_ms;
static bool utc_ready;
static bool stored_key;
static uint8_t key_bytes[32];
static unsigned random_counter;
static unsigned probes;
static char probed_id[65];
static esp_netif_t ethernet={1};
typedef struct {bool held;} mutex_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void){return calloc(1,sizeof(mutex_t));}
int xSemaphoreTake(SemaphoreHandle_t h,TickType_t t){(void)t;assert(h);mutex_t *m=h;assert(!m->held);m->held=true;return pdTRUE;}
void xSemaphoreGive(SemaphoreHandle_t h){mutex_t *m=h;assert(m&&m->held);m->held=false;}
void vTaskDelay(TickType_t t){now_ms+=t;}
int xTaskCreate(void (*f)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle){(void)f;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return pdPASS;}
uint64_t gw_core_mono_ms(void){return now_ms;}
bool gw_core_time_trusted(void){return utc_ready;}
bool gw_core_interface_ready(const char *name){return !strcmp(name,"ETHERNET");}
esp_netif_t *gw_core_uplink_netif(const char *name){return !strcmp(name,"ETHERNET")?&ethernet:NULL;}
esp_err_t gw_network_init(void){return ESP_OK;}
esp_err_t gw_network_apply(const cJSON *network){cJSON_Delete(applied);applied=cJSON_Duplicate(network,true);return ESP_OK;}
esp_err_t gw_network_scan(bool active,uint64_t deadline,cJSON **out){(void)active;(void)deadline;*out=cJSON_CreateArray();return ESP_OK;}
esp_err_t esp_flash_get_size(void *chip,uint32_t *size){(void)chip;*size=8u*1024u*1024u;return ESP_OK;}
size_t esp_psram_get_size(void){return 8u*1024u*1024u;}
void esp_fill_random(void *data,size_t n){memset(data,(int)++random_counter,n);}
esp_err_t gw_store_init(void){return ESP_OK;}
cJSON *gw_store_get_json(const char *name){return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(disk,name),true);}
esp_err_t gw_store_put_json(const char *name,const cJSON *value){cJSON_DeleteItemFromObjectCaseSensitive(disk,name);return cJSON_AddItemToObject(disk,name,cJSON_Duplicate(value,true))?ESP_OK:ESP_ERR_NO_MEM;}
esp_err_t gw_store_get(const char *name,void *out,size_t *size){if(!strcmp(name,"core/request-key")){if(!stored_key)return ESP_ERR_NOT_FOUND;if(*size<32)return ESP_ERR_INVALID_SIZE;if(out)memcpy(out,key_bytes,32);*size=32;return ESP_OK;}return cJSON_GetObjectItemCaseSensitive(disk,name)?ESP_OK:ESP_ERR_NOT_FOUND;}
esp_err_t gw_store_put(const char *name,const void *value,size_t size){assert(!strcmp(name,"core/request-key")&&size==32);memcpy(key_bytes,value,32);stored_key=true;return ESP_OK;}
esp_err_t gw_secret_get(const char *name,void *out,size_t *size){(void)name;(void)out;(void)size;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t gw_secret_put(const char *name,const void *value,size_t size){(void)name;(void)value;(void)size;return ESP_ERR_NOT_SUPPORTED;}
static int identity_hash(const unsigned char *in,size_t n,unsigned char *out){uint32_t h=2166136261u;for(size_t i=0;i<n;i++)h=(h^in[i])*16777619u;for(unsigned i=0;i<32;i++){h=h*1664525u+1013904223u;out[i]=(uint8_t)(h>>24);}return 0;}
int mbedtls_sha256(const unsigned char *in,size_t n,unsigned char *out,int is224){(void)is224;return identity_hash(in,n,out);}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type){static mbedtls_md_info_t info;info.type=type;return &info;}
int mbedtls_md(const mbedtls_md_info_t *info,const unsigned char *in,size_t n,unsigned char *out){(void)info;return identity_hash(in,n,out);}
int mbedtls_md_hmac(const mbedtls_md_info_t *info,const unsigned char *key,size_t keyn,const unsigned char *in,size_t n,unsigned char *out){(void)info;(void)key;(void)keyn;return identity_hash(in,n,out);}
static esp_err_t probe(const gw_network_probe_t *p,void *ctx){(void)ctx;assert(!((mutex_t*)core_lock)->held);assert(!p->remote&&p->deadline_mono_ms>now_ms);snprintf(probed_id,sizeof(probed_id),"%s",p->transaction_id);probes++;cJSON *snapshot=gw_core_state();assert(snapshot);cJSON_Delete(snapshot);return ESP_OK;}
static cJSON *profile(const char *ip){char text[640];snprintf(text,sizeof(text),"{\"uplinkOrder\":[\"ETHERNET\"],\"ethernet\":{\"enabled\":true,\"ipv4\":{\"mode\":\"STATIC\",\"address\":\"%s\",\"prefixLength\":24,\"gateway\":\"192.168.1.1\",\"dns\":[\"192.168.1.1\"]}},\"wifiSta\":{\"enabled\":false,\"ipv4\":{\"mode\":\"DHCP\"}}}",ip);cJSON *p=cJSON_Parse(text);assert(p);return p;}
static void boot_again(void){cJSON_Delete(state);state=NULL;free(core_lock);free(credential_lock);core_lock=credential_lock=NULL;core_fault=false;background_probe=false;work_requested=false;next_background_probe=window_deadline=0;maintenance[0]=0;current_request_id=NULL;probe_handler=NULL;cancel_requested=false;now_ms=10;assert(gw_core_init()==ESP_OK);gw_core_set_probe_handler(probe,NULL);}
static void fresh(void){cJSON_Delete(disk);disk=cJSON_CreateObject();stored_key=false;utc_ready=false;probes=0;probed_id[0]=0;boot_again();}
static void save_offline(cJSON *p,char id[65]){assert(begin(p,false,NULL,NULL,now_ms+120000,NULL)==ESP_OK);snprintf(id,65,"%s",gw_json_string(transaction(),"transactionId"));assert(commit(true)==ESP_OK);assert(gw_network_apply(p)==ESP_OK);}
int main(void){
    char id[65];cJSON *a=profile("192.168.1.20"),*b=profile("192.168.1.21");
    fresh();save_offline(a,id);now_ms+=120001;local_background();assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"localAttemptActive")));assert(probes==0);utc_ready=true;local_background();assert(probes==1&&!strcmp(id,probed_id));assert(!strcmp(gw_json_string(state,"verificationState"),"PLATFORM_CONNECTED"));assert(!strcmp(gw_json_string(state,"networkGeneration"),"1")&&!strcmp(gw_json_string(state,"localRevision"),"1"));
    fresh();replace(state,"actualNetwork",a);replace(state,"lastVerified",a);assert(save()==ESP_OK);save_offline(b,id);boot_again();assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"),a,true));assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(state,"desiredLocalNetwork"),b,true));assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"localAttemptActive")));utc_ready=true;local_background();assert(probes==0);
    fresh();save_offline(a,id);assert(begin(b,false,NULL,NULL,now_ms+120000,NULL)==ESP_OK);finish_failed("TEST_FAILURE");utc_ready=true;local_background();assert(probes==1&&!strcmp(id,probed_id));assert(!strcmp(gw_json_string(state,"networkGeneration"),"1"));
    fresh();save_offline(a,id);gw_json_set_string(transaction(),"source","REMOTE");gw_json_set_string(transaction(),"taskId","remote-1");gw_json_set_string(transaction(),"desiredRevision","7");replace(transaction(),"baseGeneration",cJSON_GetObjectItemCaseSensitive(state,"networkGeneration"));assert(commit(false)==ESP_OK);assert(network_busy());assert(!strcmp(gw_json_string(state,"verificationState"),"PLATFORM_CONNECTED"));assert(gw_core_platform_ack("wrong")==ESP_ERR_INVALID_STATE);boot_again();assert(network_busy());assert(gw_core_platform_ack("remote-1")==ESP_OK);assert(!network_busy());assert(gw_core_platform_ack("remote-1")==ESP_OK);assert(!strcmp(gw_json_string(state,"verificationState"),"PLATFORM_SYNCED"));assert(cJSON_GetObjectItemCaseSensitive(state,"lastKnownPlatformRevision")->valuedouble==7);
    cJSON_Delete(a);cJSON_Delete(b);cJSON_Delete(applied);cJSON_Delete(disk);cJSON_Delete(state);free(core_lock);free(credential_lock);puts("PASS: production network state recovery, late offline proof, committed snapshot binding, callback without mutex, remote final-ACK restart/idempotency");return 0;
}
