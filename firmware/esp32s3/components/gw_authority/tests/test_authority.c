/* Independent Python signatures verify the production two-layer trust chain. */
#include "../gw_authority.c"
#include <assert.h>
#include <stdio.h>

static cJSON *factory,*fixture;
static uint8_t public_key[32],signature[64];
static cJSON *read_json(const char *path){FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);char *raw=calloc(1,(size_t)n+1);assert(raw&&fread(raw,1,(size_t)n,f)==(size_t)n);fclose(f);cJSON *j=cJSON_Parse(raw);free(raw);assert(j);return j;}
static void replace_string(cJSON *j,const char *key,const char *value){cJSON_DeleteItemFromObjectCaseSensitive(j,key);cJSON_AddStringToObject(j,key,value);}
static void decode(const char *text,uint8_t *out,size_t n){size_t used;assert(!mbedtls_base64_decode(out,n,&used,(const uint8_t *)text,strlen(text))&&used==n);}
esp_err_t gw_factory_get_string(const char *key,char *out,size_t n){const cJSON *v=cJSON_GetObjectItemCaseSensitive(factory,key);if(!cJSON_IsString(v))return ESP_ERR_NOT_FOUND;size_t size=strlen(v->valuestring)+1;if(size>n)return ESP_ERR_INVALID_ARG;memcpy(out,v->valuestring,size);return ESP_OK;}
esp_err_t gw_factory_get_blob(const char *key,void *out,size_t *n){const void *value=NULL;size_t size=0;if(!strcmp(key,"manifest_pub")){value=public_key;size=32;}else if(!strcmp(key,"dev_signature")){value=signature;size=64;}else return ESP_ERR_NOT_FOUND;if(*n<size)return ESP_ERR_INVALID_ARG;memcpy(out,value,size);*n=size;return ESP_OK;}
static void test_verify(void){
 const char *manifest=s(fixture,"manifest"),*artifact=s(fixture,"artifact");cJSON *caps=NULL;char digest[65];
 assert(gw_authority_verify(manifest,artifact,signature,public_key,"manufacturing-test","gw_test","SN-1","GW32I33V2","2",&caps,digest)==ESP_OK);assert(cJSON_GetArraySize(caps)==3&&strlen(digest)==64);cJSON_Delete(caps);
 const char *cases[][4]={{"other","SN-1","GW32I33V2","2"},{"gw_test","other","GW32I33V2","2"},{"gw_test","SN-1","other","2"},{"gw_test","SN-1","GW32I33V2","other"}};
 for(unsigned i=0;i<4;i++){caps=(void *)1;assert(gw_authority_verify(manifest,artifact,signature,public_key,"manufacturing-test",cases[i][0],cases[i][1],cases[i][2],cases[i][3],&caps,digest)!=ESP_OK);assert(!caps&&!digest[0]);}
 uint8_t bad_signature[64];memcpy(bad_signature,signature,64);bad_signature[0]^=1;assert(gw_authority_verify(manifest,artifact,bad_signature,public_key,"manufacturing-test","gw_test","SN-1","GW32I33V2","2",&caps,digest)!=ESP_OK);
 assert(gw_authority_verify(manifest,artifact,signature,public_key,"wrong-key","gw_test","SN-1","GW32I33V2","2",&caps,digest)!=ESP_OK);
 char *changed=malloc(strlen(manifest)+2);assert(changed);sprintf(changed,"%s ",manifest);assert(gw_authority_verify(changed,artifact,signature,public_key,"manufacturing-test","gw_test","SN-1","GW32I33V2","2",&caps,digest)!=ESP_OK);free(changed);
 for(unsigned i=0;i<2;i++){const cJSON *f=cJSON_GetObjectItemCaseSensitive(fixture,i?"invalidCapability":"duplicate");uint8_t sig[64];decode(s(f,"signature"),sig,64);assert(gw_authority_verify(s(f,"manifest"),s(f,"artifact"),sig,public_key,"manufacturing-test","gw_test","SN-1","GW32I33V2","2",&caps,digest)!=ESP_OK);}
 assert(gw_authority_verify(NULL,artifact,signature,public_key,"manufacturing-test","gw_test","SN-1","GW32I33V2","2",&caps,digest)!=ESP_OK);
}
static void test_factory_state(void){
 replace_string(factory,"dev_manifest",s(fixture,"manifest"));replace_string(factory,"dev_artifact",s(fixture,"artifact"));replace_string(factory,"manifest_keyid","manufacturing-test");replace_string(factory,"gateway","gw_test");replace_string(factory,"sn","SN-1");replace_string(factory,"model","GW32I33V2");replace_string(factory,"hardware","2");replace_string(factory,"board_caps","[\"CONFIGURE\"]");
 assert(gw_authority_init()==ESP_OK);assert(gw_authority_has("NRF54_BLE_FILTER_V2"));assert(!gw_authority_board_verified("NRF54_BLE_FILTER_V2"));assert(gw_authority_board_verified("CONFIGURE"));assert(strlen(gw_authority_digest())==64);
 cJSON_DeleteItemFromObjectCaseSensitive(factory,"dev_artifact");assert(gw_authority_init()!=ESP_OK);assert(!gw_authority_has("CONFIGURE")&&!gw_authority_digest()[0]);replace_string(factory,"dev_artifact",s(fixture,"artifact"));
 replace_string(factory,"board_caps","{}");assert(gw_authority_init()==ESP_OK);assert(!gw_authority_board_verified("CONFIGURE"));
 cJSON *keys=cJSON_CreateObject();cJSON_AddStringToObject(keys,"firmware-test",s(fixture,"publicKey"));char *text=cJSON_PrintUnformatted(keys);replace_string(factory,"artifact_keys",text);free(text);cJSON_Delete(keys);
 const cJSON *bundle=cJSON_GetObjectItemCaseSensitive(fixture,"factoryFirmware");text=cJSON_PrintUnformatted(bundle);replace_string(factory,"gw_fwproof",text);free(text);
 cJSON *candidate=gw_authority_factory_image("GATEWAY");assert(candidate&&!strcmp(s(candidate,"source"),"FACTORY")&&!strcmp(s(candidate,"artifactId"),"artifact-local")&&!strcmp(s(candidate,"component"),"GATEWAY"));cJSON_Delete(candidate);
 assert(!gw_authority_factory_image("NRF54"));replace_string(factory,"model","wrong");assert(!gw_authority_factory_image("GATEWAY"));replace_string(factory,"model","GW32I33V2");replace_string(factory,"hardware","wrong");assert(!gw_authority_factory_image("GATEWAY"));replace_string(factory,"hardware","2");
 replace_string(factory,"artifact_keys","{}");assert(!gw_authority_factory_image("GATEWAY"));assert(!gw_authority_factory_image("UNKNOWN"));
}
int main(int argc,char **argv){assert(argc==2);fixture=read_json(argv[1]);factory=cJSON_CreateObject();decode(s(fixture,"publicKey"),public_key,32);decode(s(fixture,"signature"),signature,64);test_verify();test_factory_state();cJSON_Delete(factory);cJSON_Delete(fixture);cJSON_Delete(authorized);cJSON_Delete(board);puts("PASS authority: independent Ed25519 signatures, exact bytes, gateway/SN/model binding and missing evidence denial");return 0;}
