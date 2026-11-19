/* Production synchronization state machine; only device I/O is simulated. */
#include "../gw_sync.c"
#include <assert.h>

static cJSON *database,*fixture,*http_snapshot,*http_history,*last_report,*last_caps;
static bool store_fail,local_override,authority_enabled=true,nrf_live=true;
static bool receipt_wrong,partition_error,dfu_error,dfu_confirmed=true;
static unsigned calls_identity,flip_at,verify_calls,locks,releases;
static uint32_t hello_bits;
static uint64_t mono=100;
static uint8_t image_data[128];
static esp_partition_t running={sizeof(image_data),"ota_0"};
static esp_app_desc_t description={"1.0"};
static esp_ota_img_states_t image_state=ESP_OTA_IMG_VALID;

static cJSON *read_json(const char *path){FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);char *raw=calloc(1,(size_t)n+1);assert(raw&&fread(raw,1,(size_t)n,f)==(size_t)n);fclose(f);cJSON *j=cJSON_Parse(raw);free(raw);assert(j);return j;}
static void set_string(cJSON *j,const char *key,const char *value){cJSON_DeleteItemFromObjectCaseSensitive(j,key);cJSON_AddStringToObject(j,key,value);}
static void set_number(cJSON *j,const char *key,double value){cJSON_DeleteItemFromObjectCaseSensitive(j,key);cJSON_AddNumberToObject(j,key,value);}
static void seal(cJSON *j){cJSON_DeleteItemFromObjectCaseSensitive(j,"digest");char hash[65];assert(gw_core_network_snapshot_id(j,hash)==ESP_OK);cJSON_AddStringToObject(j,"digest",hash);}
static cJSON *new_snapshot(void){return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(fixture,"snapshot"),true);}
static cJSON *group(cJSON *j,const char *name){return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"groups"),name);}
static cJSON *proof(const char *component){uint8_t sha[32];char digest[65];assert(!mbedtls_sha256(image_data,sizeof(image_data),sha,0));hex(sha,32,digest);cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"phase","CONFIRMED");cJSON_AddStringToObject(j,"component",component);cJSON_AddStringToObject(j,"version","1.0");cJSON_AddStringToObject(j,"sha256",digest);cJSON_AddStringToObject(j,"artifactId","artifact-image");cJSON_AddNumberToObject(j,"size",sizeof(image_data));return j;}

esp_err_t gw_store_put_json(const char *key,const cJSON *j){if(store_fail)return ESP_FAIL;cJSON_DeleteItemFromObjectCaseSensitive(database,key);assert(cJSON_AddItemToObject(database,key,cJSON_Duplicate(j,true)));return ESP_OK;}
cJSON *gw_store_get_json(const char *key){return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(database,key),true);}
cJSON *gw_core_state(void){cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"localOverride",local_override);return j;}
uint64_t gw_core_mono_ms(void){return mono;}
uint64_t gw_core_time_deadline(uint64_t expiry){return expiry;}
esp_err_t gw_core_maintenance_acquire(const char *owner){assert(!strcmp(owner,"component-inventory"));locks++;return ESP_OK;}
void gw_core_maintenance_release(const char *owner){assert(!strcmp(owner,"component-inventory"));releases++;}
esp_netif_t *gw_core_uplink_netif(const char *name){(void)name;return (esp_netif_t *)1;}
esp_err_t gw_credential_password(const char *ref,const char *ssid,const char *security,char password[65]){(void)ref;(void)ssid;(void)security;snprintf(password,65,"test-password");return ESP_OK;}
bool gw_authority_has(const char *cap){(void)cap;return authority_enabled;}
bool gw_authority_board_verified(const char *cap){(void)cap;return true;}
const char *gw_authority_digest(void){return "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";}
cJSON *gw_authority_factory_image(const char *component){(void)component;return NULL;}
bool gw_capture_upload_valid(const cJSON *j){return cJSON_IsObject(j)&&cJSON_GetArraySize(j)==2&&revision(cJSON_GetObjectItemCaseSensitive(j,"batchMaxRecords"))&&revision(cJSON_GetObjectItemCaseSensitive(j,"flushIntervalMs"));}
const esp_app_desc_t *esp_app_get_description(void){return &description;}
const esp_partition_t *esp_ota_get_running_partition(void){return &running;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *state){assert(p==&running);*state=image_state;return ESP_OK;}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t n){assert(p==&running);if(partition_error||offset+n>sizeof(image_data))return ESP_FAIL;memcpy(out,image_data+offset,n);return ESP_OK;}
bool gw_nrf_identity(uint8_t boot[16],uint32_t *caps,char version[64]){calls_identity++;if(!nrf_live)return false;if(boot)memset(boot,flip_at&&calls_identity>=flip_at?0xbb:0xaa,16);if(caps)*caps=hello_bits;if(version)snprintf(version,64,"1.0");return true;}
esp_err_t gw_nrf_command(uint8_t op,const void *args,size_t n,uint8_t *status,void *out,size_t *out_size,int64_t deadline){assert((uint64_t)deadline>mono);*status=dfu_error?1:0;if(op==0x37){assert(n==36&&gw_get_u32(args)==sizeof(image_data));verify_calls++;uint8_t sha[32];assert(!mbedtls_sha256(image_data,sizeof(image_data),sha,0));if(memcmp((const uint8_t *)args+4,sha,32))*status=1;*out_size=0;}else{assert(op==0x33&&n==0&&*out_size>=62);memset(out,0,62);((uint8_t *)out)[9]=dfu_confirmed?1:0;*out_size=62;}return ESP_OK;}
esp_err_t gw_http_json(const char *path,const char *method,const cJSON *body,const char *iface,uint64_t deadline,cJSON **out){(void)iface;assert(deadline>mono);*out=NULL;
 if(!strcmp(path,"/api/v1/gateway-runtime-capabilities")){assert(!strcmp(method,"PUT"));cJSON_Delete(last_caps);last_caps=cJSON_Duplicate(body,true);*out=cJSON_CreateObject();return ESP_OK;}
 if(!strncmp(path,"/api/v1/gateway-configuration-snapshot",strlen("/api/v1/gateway-configuration-snapshot"))){assert(!strcmp(method,"GET"));const cJSON *source=strstr(path,"snapshotId=")?http_history:http_snapshot;if(!source)return ESP_ERR_NOT_FOUND;*out=cJSON_Duplicate(source,true);return ESP_OK;}
 if(!strcmp(path,"/api/v1/gateway-component-challenges")){assert(!strcmp(method,"POST"));*out=cJSON_Parse("{\"schemaVersion\":\"1.0\",\"challengeId\":\"challenge-1\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"inventoryGeneration\":\"0\",\"expiresAtUnixMs\":\"100000\"}");cJSON_AddStringToObject(*out,"component",s(body,"component"));return ESP_OK;}
 if(!strcmp(path,"/api/v1/gateway-components")){assert(!strcmp(method,"PUT"));cJSON_Delete(last_report);last_report=cJSON_Duplicate(body,true);*out=identity("ns_test","gw_test");cJSON_AddStringToObject(*out,"component",s(body,"component"));cJSON_AddStringToObject(*out,"challengeId",receipt_wrong?"wrong-challenge":"challenge-1");cJSON_AddStringToObject(*out,"inventoryGeneration","1");return ESP_OK;}
 assert(!"unexpected HTTP operation");return ESP_FAIL;
}

static void test_snapshot_validation(void){
 cJSON *j=new_snapshot();assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")==ESP_OK);
 char hash[65];assert(gw_core_network_snapshot_id(cJSON_GetObjectItemCaseSensitive(fixture,"maxInteger"),hash)==ESP_OK);assert(!strcmp(hash,s(fixture,"maxIntegerDigest")));
 assert(gw_cloud_snapshot_check(j,"other","gw_test")!=ESP_OK);assert(gw_cloud_snapshot_check(j,"ns_test","other")!=ESP_OK);
 set_string(j,"digest","invalid");assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);seal(j);
 cJSON_AddBoolToObject(j,"unknown",true);seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON_DeleteItemFromObjectCaseSensitive(j,"blockedReasons");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON_AddStringToObject(j,"generation","1");assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(group(j,"nrf54Scan"),"value"),"beaconFilterV2");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(j,"groups"),"network");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();set_number(group(j,"nrf54Scan"),"revision",0);seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")==ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON *network=cJSON_GetObjectItemCaseSensitive(group(j,"network"),"value");cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(network,"wifiSta"),"password","not-allowed");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();network=cJSON_GetObjectItemCaseSensitive(group(j,"network"),"value");cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(network,"ethernet"),"ipv4");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();cJSON *advertising=cJSON_GetObjectItemCaseSensitive(group(j,"esp32Advertising"),"value");cJSON_AddBoolToObject(advertising,"unknown",true);seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();advertising=cJSON_GetObjectItemCaseSensitive(group(j,"esp32Advertising"),"value");set_string(advertising,"mode","UNSUPPORTED");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
 j=new_snapshot();advertising=cJSON_GetObjectItemCaseSensitive(group(j,"esp32Advertising"),"value");set_string(advertising,"advDataBase64","AwE=");seal(j);assert(gw_cloud_snapshot_check(j,"ns_test","gw_test")!=ESP_OK);cJSON_Delete(j);
}
static void test_candidate_order_and_recovery(void){
 http_snapshot=new_snapshot();snapshot("ns_test","gw_test");cJSON *saved=gw_store_get_json("cloud/recoverySnapshot");assert(saved&&!strcmp(s(saved,"snapshotId"),"snapshot-1"));cJSON_Delete(saved);
 set_string(http_snapshot,"generation","9007199254740992");set_string(http_snapshot,"snapshotId","stale");seal(http_snapshot);snapshot("ns_test","gw_test");saved=gw_store_get_json("cloud/recoverySnapshot");assert(!strcmp(s(saved,"snapshotId"),"snapshot-1"));cJSON_Delete(saved);
 set_string(http_snapshot,"generation","9007199254740993");seal(http_snapshot);snapshot("ns_test","gw_test");saved=gw_store_get_json("cloud/recoverySnapshot");assert(!strcmp(s(saved,"snapshotId"),"snapshot-1"));cJSON_Delete(saved);
 set_string(http_snapshot,"generation","9007199254740994");set_string(http_snapshot,"snapshotId","snapshot-2");cJSON_ReplaceItemInObjectCaseSensitive(http_snapshot,"complete",cJSON_CreateFalse());cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(http_snapshot,"blockedReasons"),cJSON_CreateString("UNKNOWN_TASK"));seal(http_snapshot);snapshot("ns_test","gw_test");saved=gw_store_get_json("cloud/recoverySnapshot");assert(!strcmp(s(saved,"snapshotId"),"snapshot-1"));cJSON_Delete(saved);saved=gw_store_get_json("cloud/snapshotView");assert(!strcmp(s(saved,"snapshotId"),"snapshot-2"));cJSON_Delete(saved);
 http_history=new_snapshot();cJSON *command=cJSON_Parse("{\"parameters\":{\"sourceSnapshotId\":\"snapshot-1\",\"desiredRevision\":1000000000001,\"configuration\":{\"upload\":{\"batchMaxRecords\":32,\"flushIntervalMs\":1000}}}}");assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)==ESP_OK);
 cJSON_DeleteItemFromObjectCaseSensitive(database,"cloud/recoverySnapshot");assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)==ESP_OK);
 local_override=true;assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);local_override=false;
 cJSON *p=cJSON_GetObjectItemCaseSensitive(command,"parameters");set_number(p,"desiredRevision",1000000000000);assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);set_number(p,"desiredRevision",1000000000001);
 cJSON *configuration=cJSON_GetObjectItemCaseSensitive(p,"configuration");set_number(cJSON_GetObjectItemCaseSensitive(configuration,"upload"),"batchMaxRecords",33);assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);set_number(cJSON_GetObjectItemCaseSensitive(configuration,"upload"),"batchMaxRecords",32);cJSON_AddObjectToObject(configuration,"network");assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);cJSON_DeleteItemFromObjectCaseSensitive(configuration,"network");
 authority_enabled=false;assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);authority_enabled=true;store_fail=true;assert(gw_cloud_recovery_check(command,"ns_test","gw_test",5000)!=ESP_OK);store_fail=false;
 cJSON_Delete(command);cJSON_Delete(http_snapshot);http_snapshot=NULL;cJSON_Delete(http_history);http_history=NULL;
}
static void test_inventory_and_capabilities(void){
 cJSON *p=proof("GATEWAY");uint8_t boot[16];char version[64];assert(actual_image("GATEWAY",p,5000,boot,version));set_string(p,"sha256","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");assert(!actual_image("GATEWAY",p,5000,boot,version));cJSON_Delete(p);
 p=proof("GATEWAY");assert(gw_store_put_json("ota/proof/GATEWAY",p)==ESP_OK);cJSON_Delete(p);inventory("GATEWAY","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"CONFIRMED_ARTIFACT"));assert(cJSON_HasObjectItem(database,"cloud/component/GATEWAY"));
 cJSON_DeleteItemFromObjectCaseSensitive(database,"cloud/component/GATEWAY");receipt_wrong=true;inventory("GATEWAY","ns_test","gw_test","11111111111111111111111111111111");assert(!cJSON_HasObjectItem(database,"cloud/component/GATEWAY"));receipt_wrong=false;
 image_state=ESP_OTA_IMG_PENDING_VERIFY;inventory("GATEWAY","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"VERSION_ONLY")&&!cJSON_HasObjectItem(last_report,"imageSha256")&&!cJSON_HasObjectItem(last_report,"bootConfirmed"));image_state=ESP_OTA_IMG_VALID;
 p=proof("NRF54");assert(gw_store_put_json("ota/proof/NRF54",p)==ESP_OK);cJSON_Delete(p);hello_bits=(1u<<12);calls_identity=0;inventory("NRF54","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"CONFIRMED_ARTIFACT")&&verify_calls>0);
 calls_identity=0;flip_at=2;dfu_error=true;inventory("NRF54","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"UNKNOWN")&&!cJSON_HasObjectItem(last_report,"controllerBootId")&&!cJSON_HasObjectItem(last_report,"firmwareVersion"));flip_at=0;dfu_error=false;
 calls_identity=0;dfu_confirmed=false;inventory("NRF54","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"VERSION_ONLY"));dfu_confirmed=true;
 nrf_live=false;inventory("NRF54","ns_test","gw_test","11111111111111111111111111111111");assert(!strcmp(s(last_report,"evidenceLevel"),"UNKNOWN"));capabilities("ns_test","gw_test","11111111111111111111111111111111");assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(last_caps,"nrfBootId"))&&cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(last_caps,"nrfHelloCapabilities"))==0);
 nrf_live=true;hello_bits=(1u<<2)|(1u<<4)|(1u<<9)|(1u<<10)|(1u<<11);capabilities("ns_test","gw_test","11111111111111111111111111111111");const cJSON *hello=cJSON_GetObjectItemCaseSensitive(last_caps,"nrfHelloCapabilities");bool adv=false,discovery=false,peripheral=false;for(const cJSON *v=hello->child;v;v=v->next){adv|=!strcmp(v->valuestring,"NRF54_BLE_ADV");discovery|=!strcmp(v->valuestring,"NRF54_BLE_GATT_SERVICE_DISCOVERY");peripheral|=!strcmp(v->valuestring,"NRF54_BLE_PERIPHERAL");}assert(adv&&discovery&&peripheral);assert(locks==releases);
}
int main(int argc,char **argv){assert(argc==2);fixture=read_json(argv[1]);database=cJSON_CreateObject();for(size_t i=0;i<sizeof(image_data);i++)image_data[i]=(uint8_t)i;test_snapshot_validation();test_candidate_order_and_recovery();test_inventory_and_capabilities();cJSON_Delete(database);cJSON_Delete(fixture);cJSON_Delete(last_report);cJSON_Delete(last_caps);puts("PASS sync: canonical SHA, complete groups, generation, historical recovery, image proof and boot changes");return 0;}
