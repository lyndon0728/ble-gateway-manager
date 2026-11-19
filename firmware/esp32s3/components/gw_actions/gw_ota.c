#include "gw_actions_internal.h"
#include "gw_nrf.h"
#include "gw_http.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_KEY "ota/active"
static esp_timer_handle_t trial_timer;
static uint64_t trial_deadline;
static void set(cJSON *j,const char *k,cJSON *v){cJSON_DeleteItemFromObjectCaseSensitive(j,k);cJSON_AddItemToObject(j,k,v);}
static esp_err_t phase(cJSON *j,const char *value){set(j,"phase",cJSON_CreateString(value));return gw_store_put_json(OTA_KEY,j);}
static bool safe_id(const char *s){size_t n=strlen(s);if(!n||n>128)return false;for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'))return false;return true;}
static bool member(const cJSON *j,const char *key,const char *value,bool required){
    const cJSON *list=cJSON_GetObjectItemCaseSensitive(j,key),*v;
    if(!list)return !required;if(!cJSON_IsArray(list)||cJSON_GetArraySize(list)>128)return false;
    if(!cJSON_GetArraySize(list))return !required;
    cJSON_ArrayForEach(v,list)if(cJSON_IsString(v)&&!strcmp(v->valuestring,value))return true;return false;
}
static bool unique_keys(const cJSON *j,unsigned depth){
    if(depth>8)return false;const cJSON *a,*b;
    cJSON_ArrayForEach(a,j){if(cJSON_IsObject(j)){if(!a->string)return false;for(b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return false;}
        if((cJSON_IsObject(a)||cJSON_IsArray(a))&&!unique_keys(a,depth+1))return false;}return true;
}
/* Exact raw manifest bytes are authenticated with a factory-provisioned
 * release key. The network cannot install or replace this trust anchor. */
static esp_err_t manifest(const cJSON *command,uint64_t deadline,cJSON **out){
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(command,"parameters");const char *artifact=gw_action_string(p,"artifactId"),*component=gw_action_string(p,"component");
    if(!*component)component="GATEWAY";if(!safe_id(artifact)||(strcmp(component,"NRF54")&&strcmp(component,"GATEWAY")))return ESP_ERR_INVALID_ARG;
    char path[192];snprintf(path,sizeof(path),"/api/v1/artifacts/%s/verification",artifact);cJSON *response=NULL,*m=NULL;
    esp_err_t e=gw_http_json(path,"GET",NULL,NULL,deadline,&response);if(e!=ESP_OK)return e;
    const char *raw=gw_action_string(response,"manifest"),*signature=gw_action_string(response,"signature");size_t length=strlen(raw);
    if(!length||length>65536||strstr(raw,"\\u0000")){e=ESP_ERR_INVALID_RESPONSE;goto done;}
    const char *end=NULL;m=cJSON_ParseWithLengthOpts(raw,length+1,&end,true);
    if(!cJSON_IsObject(m)||!unique_keys(m,0)){e=ESP_ERR_INVALID_RESPONSE;goto done;}
    char keys[8192];if(gw_factory_get_string("artifact_keys",keys,sizeof(keys))!=ESP_OK){e=ESP_ERR_INVALID_STATE;goto done;}
    cJSON *trusted=cJSON_Parse(keys);memset(keys,0,sizeof(keys));
    const char *encoded=gw_action_string(trusted,gw_action_string(m,"keyId"));uint8_t public_key[32],sig[64];size_t pn=0,sn=0;
    bool decoded=mbedtls_base64_decode(public_key,sizeof(public_key),&pn,(const unsigned char*)encoded,strlen(encoded))==0&&pn==32&&mbedtls_base64_decode(sig,sizeof(sig),&sn,(const unsigned char*)signature,strlen(signature))==0&&sn==64;
    cJSON_Delete(trusted);
    if(!decoded||crypto_ed25519_check(sig,public_key,(const uint8_t*)raw,length)){e=ESP_ERR_INVALID_CRC;goto done;}
    const char *signed_component=gw_action_string(m,"component");if(!*signed_component)signed_component="GATEWAY";
    char model[129],hardware[129],current[65];uint8_t hash[32];
    if(gw_factory_get_string("model",model,sizeof(model))!=ESP_OK||gw_factory_get_string("hardware",hardware,sizeof(hardware))!=ESP_OK){e=ESP_ERR_INVALID_STATE;goto done;}
    if(!strcmp(component,"NRF54")){if(!gw_nrf_identity(NULL,NULL,current)){e=ESP_ERR_INVALID_STATE;goto done;}}
    else snprintf(current,sizeof(current),"%s",esp_app_get_description()->version);
    if(strcmp(gw_action_string(m,"kind"),"FIRMWARE")||strcmp(component,signed_component)||!gw_action_hex(gw_action_string(m,"sha256"),hash,32)||
       !*gw_action_string(m,"name")||!*gw_action_string(m,"version")||strlen(gw_action_string(m,"version"))>64||
       strcmp(gw_action_string(m,"version"),gw_action_string(p,"expectedFirmwareVersion"))||
       !member(m,"compatibleModels",model,true)||!member(m,"hardwareVersions",hardware,false)||!member(m,"fromFirmwareVersions",current,false)||
       (!strcmp(component,"NRF54")&&strcmp(gw_action_string(m,"imageFormat"),"MCUBOOT_SIGNED_BIN"))){e=ESP_ERR_INVALID_ARG;goto done;}
    *out=m;m=NULL;e=ESP_OK;
done:cJSON_Delete(m);cJSON_Delete(response);return e;
}
typedef struct {
    const esp_partition_t *partition;esp_ota_handle_t handle;bool gateway;
    mbedtls_sha256_context hash;size_t offset,total;uint8_t pending[16];size_t pending_length;
} download_t;
static esp_err_t download_sink(const uint8_t *data,size_t n,void *context){
    download_t *d=context;if(d->total+n>d->partition->size)return ESP_ERR_INVALID_SIZE;
    if(mbedtls_sha256_update(&d->hash,data,n))return ESP_FAIL;d->total+=n;
    if(d->gateway)return esp_ota_write(d->handle,data,n);
    while(n){size_t take=16-d->pending_length;if(take>n)take=n;memcpy(d->pending+d->pending_length,data,take);d->pending_length+=take;data+=take;n-=take;
        if(d->pending_length==16){esp_err_t e=esp_partition_write(d->partition,d->offset,d->pending,16);if(e!=ESP_OK)return e;d->offset+=16;d->pending_length=0;}}
    return ESP_OK;
}
static esp_err_t partition_hash(const esp_partition_t *p,size_t length,uint8_t hash[32]){
    if(!p||length>p->size)return ESP_ERR_INVALID_SIZE;uint8_t *buffer=malloc(4096);if(!buffer)return ESP_ERR_NO_MEM;
    mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);esp_err_t e=mbedtls_sha256_starts(&ctx,0)?ESP_FAIL:ESP_OK;
    for(size_t offset=0;e==ESP_OK&&offset<length;){size_t n=length-offset;if(n>4096)n=4096;e=esp_partition_read(p,offset,buffer,n);if(e==ESP_OK&&mbedtls_sha256_update(&ctx,buffer,n))e=ESP_FAIL;offset+=n;}
    if(e==ESP_OK&&mbedtls_sha256_finish(&ctx,hash))e=ESP_FAIL;mbedtls_sha256_free(&ctx);free(buffer);return e;
}
static esp_err_t download(cJSON *j,uint64_t deadline){
    bool gateway=!strcmp(gw_action_string(j,"component"),"GATEWAY");
    const esp_partition_t *p=gateway?esp_ota_get_next_update_partition(NULL):esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x41,"nrf_image");
    if(!p)return ESP_ERR_NOT_FOUND;download_t d={.partition=p,.gateway=gateway};mbedtls_sha256_init(&d.hash);
    esp_err_t e=phase(j,"DOWNLOADING");if(e!=ESP_OK)goto done;
    e=gateway?esp_ota_begin(p,OTA_SIZE_UNKNOWN,&d.handle):esp_partition_erase_range(p,0,p->size);if(e!=ESP_OK)goto done;
    if(mbedtls_sha256_starts(&d.hash,0)){e=ESP_FAIL;goto abort;}
    char path[192];snprintf(path,sizeof(path),"/api/v1/artifacts/%s/content",gw_action_string(j,"artifactId"));size_t received=0;
    e=gw_http_stream(path,deadline,p->size,download_sink,&d,&received);if(e!=ESP_OK)goto abort;
    uint8_t hash[32],expected[32];if(received!=d.total||d.total<32||mbedtls_sha256_finish(&d.hash,hash)||!gw_action_hex(gw_action_string(j,"sha256"),expected,32)||memcmp(hash,expected,32)){e=ESP_ERR_INVALID_CRC;goto abort;}
    if(gateway){e=esp_ota_end(d.handle);d.handle=0;if(e!=ESP_OK)goto done;
        esp_app_desc_t desc;if(esp_ota_get_partition_description(p,&desc)!=ESP_OK||strcmp(desc.version,gw_action_string(j,"version"))){e=ESP_ERR_INVALID_VERSION;goto done;}}
    else if(d.pending_length){memset(d.pending+d.pending_length,0xff,16-d.pending_length);e=esp_partition_write(p,d.offset,d.pending,16);if(e!=ESP_OK)goto done;}
    e=partition_hash(p,d.total,hash);if(e!=ESP_OK||memcmp(hash,expected,32)){e=ESP_ERR_INVALID_CRC;goto done;}
    set(j,"size",cJSON_CreateNumber((double)d.total));set(j,"partition",cJSON_CreateString(p->label));e=phase(j,"VERIFIED_ARTIFACT");goto done;
abort:if(gateway&&d.handle)esp_ota_abort(d.handle);
done:mbedtls_sha256_free(&d.hash);return e;
}
static esp_err_t command(uint8_t opcode,const void *args,size_t size,uint8_t *data,size_t *length,uint64_t deadline){
    uint8_t status=255;esp_err_t e=gw_nrf_command(opcode,args,size,&status,data,length,(int64_t)deadline);
    if(e==ESP_OK&&status)e=status==GW_BUSY?ESP_ERR_NOT_FINISHED:status==GW_UNSUPPORTED?ESP_ERR_NOT_SUPPORTED:ESP_FAIL;return e;
}
static esp_err_t dfu_status(uint8_t status[62],uint64_t deadline){size_t n=62;esp_err_t e=command(0x33,NULL,0,status,&n,deadline);return e==ESP_OK&&n!=62?ESP_ERR_INVALID_RESPONSE:e;}
static esp_err_t nrf_health(uint64_t deadline){
    uint8_t status[71];size_t n=sizeof(status);esp_err_t e=command(3,NULL,0,status,&n,deadline);
    return e==ESP_OK&&n!=71?ESP_ERR_INVALID_RESPONSE:e;
}
static cJSON *result(cJSON *j,const uint8_t boot[16]){
    cJSON *o=cJSON_CreateObject();if(!o)return NULL;cJSON_AddStringToObject(o,"component",gw_action_string(j,"component"));
    cJSON_AddStringToObject(o,"firmwareVersion",gw_action_string(j,"version"));cJSON_AddStringToObject(o,"imageSha256",gw_action_string(j,"sha256"));cJSON_AddBoolToObject(o,"bootConfirmed",true);
    if(boot){char hex[33];gw_action_hex_encode(boot,16,hex);cJSON_AddStringToObject(o,"nrfBootId",hex);}return o;
}
static bool session_matches(const uint8_t status[62],cJSON *j){
    uint8_t hash[32];return gw_get_u64(status)==strtoull(gw_action_string(j,"session"),NULL,10)&&
      gw_get_u32(status+14)==(uint32_t)cJSON_GetObjectItemCaseSensitive(j,"size")->valuedouble&&
      gw_action_hex(gw_action_string(j,"sha256"),hash,32)&&!memcmp(status+26,hash,32);
}
static esp_err_t nrf_trial(cJSON *j,uint64_t deadline,cJSON **details){
    uint8_t old_boot[16],boot[16],status[62],args[36];char version[64];
    if(!gw_action_hex(gw_action_string(j,"previousBoot"),old_boot,16))return ESP_ERR_INVALID_STATE;
    while(gw_core_mono_ms()<deadline){
        if(gw_nrf_identity(boot,NULL,version)&&memcmp(boot,old_boot,16))break;
        (void)gw_nrf_rehandshake((int64_t)(gw_core_mono_ms()+2000<deadline?gw_core_mono_ms()+2000:deadline));vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(!gw_nrf_identity(boot,NULL,version)||!memcmp(boot,old_boot,16))return ESP_ERR_NOT_FINISHED;
    esp_err_t e=dfu_status(status,deadline);if(e!=ESP_OK)return e;
    if(status[8]!=0)return ESP_ERR_INVALID_STATE;
    if(strcmp(version,gw_action_string(j,"version"))){if(!status[9])return ESP_ERR_NOT_FINISHED;phase(j,"ROLLED_BACK");return ESP_ERR_INVALID_VERSION;}
    if(!status[9]){uint32_t remaining=gw_get_u32(status+58);if(remaining<500)return ESP_ERR_TIMEOUT;uint64_t budget=gw_core_mono_ms()+remaining-200;if(deadline>budget)deadline=budget;}
    if((e=nrf_health(deadline))!=ESP_OK)return e;
    gw_put_u32(args,(uint32_t)cJSON_GetObjectItemCaseSensitive(j,"size")->valuedouble);gw_action_hex(gw_action_string(j,"sha256"),args+4,32);
    size_t n=0;if((e=command(0x37,args,sizeof(args),NULL,&n,deadline))!=ESP_OK)return e;
    if((e=phase(j,"CONFIRMING"))!=ESP_OK)return e;
    n=0;if((e=command(0x36,NULL,0,NULL,&n,deadline))!=ESP_OK)return e;
    if((e=dfu_status(status,deadline))!=ESP_OK||!status[9])return e==ESP_OK?ESP_ERR_INVALID_RESPONSE:e;
    char hex[33];gw_action_hex_encode(boot,16,hex);set(j,"confirmedBoot",cJSON_CreateString(hex));
    if((e=phase(j,"CONFIRMED"))!=ESP_OK)return ESP_ERR_TIMEOUT;*details=result(j,boot);return *details?ESP_OK:ESP_ERR_NO_MEM;
}
static esp_err_t nrf_transfer(cJSON *j,uint64_t deadline,cJSON **details){
    uint8_t boot[16],old[16],status[62],hash[32];uint32_t caps=0;
    if(!gw_nrf_identity(boot,&caps,NULL)||!(caps&(1u<<12)))return ESP_ERR_NOT_SUPPORTED;
    if(!gw_action_hex(gw_action_string(j,"previousBoot"),old,16)||memcmp(boot,old,16))return nrf_trial(j,deadline,details);
    esp_err_t e=dfu_status(status,deadline);if(e!=ESP_OK)return e;
    const char *p=gw_action_string(j,"phase");
    if(status[8]==0){
        if(strcmp(p,"VERIFIED_ARTIFACT")&&strcmp(p,"BEGIN_INTENT"))return ESP_ERR_INVALID_STATE;
        if(!status[9])return ESP_ERR_INVALID_STATE;
        uint32_t size=(uint32_t)cJSON_GetObjectItemCaseSensitive(j,"size")->valuedouble;if(size>gw_get_u32(status+18))return ESP_ERR_INVALID_SIZE;
        uint8_t begin[44];gw_put_u64(begin,strtoull(gw_action_string(j,"session"),NULL,10));gw_put_u32(begin+8,size);gw_action_hex(gw_action_string(j,"sha256"),begin+12,32);
        if((e=phase(j,"BEGIN_INTENT"))!=ESP_OK)return e;
        size_t n=62;e=command(0x30,begin,sizeof(begin),status,&n,deadline);if(e!=ESP_OK)return e;if(n!=62)return ESP_ERR_INVALID_RESPONSE;
    }
    if(!session_matches(status,j))return ESP_ERR_INVALID_STATE;
    if(status[8]==3||status[8]==4)return ESP_FAIL;
    const esp_partition_t *cache=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x41,"nrf_image");size_t size=(size_t)cJSON_GetObjectItemCaseSensitive(j,"size")->valuedouble;
    uint8_t actual[32];gw_action_hex(gw_action_string(j,"sha256"),hash,32);if(partition_hash(cache,size,actual)!=ESP_OK||memcmp(actual,hash,32))return ESP_ERR_INVALID_CRC;
    if(status[8]==1){
        if((e=phase(j,"TRANSFERRING"))!=ESP_OK)return e;
        uint8_t chunk[1038];gw_put_u64(chunk,strtoull(gw_action_string(j,"session"),NULL,10));uint32_t offset=gw_get_u32(status+10);if(offset>size||(offset&&offset<32))return ESP_ERR_INVALID_RESPONSE;
        while(offset<size){if(gw_core_mono_ms()+100>=deadline)return ESP_ERR_TIMEOUT;size_t n=size-offset;if(n>1024)n=1024;
            gw_put_u32(chunk+8,offset);gw_put_u16(chunk+12,(uint16_t)n);if((e=esp_partition_read(cache,offset,chunk+14,n))!=ESP_OK)return e;
            size_t reply=62;e=command(0x31,chunk,n+14,status,&reply,deadline);
            if(e!=ESP_OK){/* Query authoritative offset before any retry. */if(dfu_status(status,deadline)!=ESP_OK)return ESP_ERR_TIMEOUT;}
            if(!session_matches(status,j)||status[8]!=1)return ESP_ERR_INVALID_RESPONSE;
            uint32_t accepted=gw_get_u32(status+10);if(accepted!=offset+n)return ESP_ERR_TIMEOUT;offset=accepted;
        }
        uint8_t session[8];gw_put_u64(session,strtoull(gw_action_string(j,"session"),NULL,10));if((e=phase(j,"FINISH_INTENT"))!=ESP_OK)return e;
        size_t n=62;e=command(0x32,session,sizeof(session),status,&n,deadline);if(e!=ESP_OK)return e;
        if(n!=62||!session_matches(status,j)||status[8]!=2)return ESP_ERR_INVALID_RESPONSE;
    }
    if(status[8]==2){
        if((e=phase(j,"READY"))!=ESP_OK)return e;
        /* FIFO bytes remain owned by nRF until durable capture ACK. Never
         * reboot the controller while its queue still contains records. */
        for(;;){uint8_t s[71];size_t n=sizeof(s);if((e=command(3,NULL,0,s,&n,deadline))!=ESP_OK)return e;
            if(n!=71)return ESP_ERR_INVALID_RESPONSE;if(!gw_get_u16(s+17)&&!gw_get_u32(s+19))break;
            if(gw_core_mono_ms()+200>=deadline)return ESP_ERR_NOT_FINISHED;vTaskDelay(pdMS_TO_TICKS(100));}
        if((e=phase(j,"REBOOT_INTENT"))!=ESP_OK)return e;uint8_t session[8];gw_put_u64(session,strtoull(gw_action_string(j,"session"),NULL,10));size_t n=0;
        e=command(0x35,session,sizeof(session),NULL,&n,deadline);if(e!=ESP_OK&&e!=ESP_ERR_TIMEOUT)return e;
    }else if(status[8]!=5)return ESP_ERR_INVALID_STATE;
    return nrf_trial(j,deadline,details);
}
static void trial_expired(void *arg){(void)arg;esp_ota_mark_app_invalid_rollback_and_reboot();esp_restart();}
esp_err_t gw_ota_init(void){
    cJSON *j=gw_store_get_json(OTA_KEY);
    if(j&&strcmp(gw_action_string(j,"phase"),"CONFIRMED")&&strcmp(gw_action_string(j,"phase"),"FAILED")&&strcmp(gw_action_string(j,"phase"),"ROLLED_BACK"))gw_core_maintenance_acquire(gw_action_string(j,"taskId"));
    cJSON_Delete(j);
    esp_ota_img_states_t state;const esp_partition_t *running=esp_ota_get_running_partition();
    if(esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY)return ESP_OK;
    trial_deadline=gw_core_mono_ms()+90000;
    const esp_timer_create_args_t args={.callback=trial_expired,.name="ota_trial"};esp_err_t e=esp_timer_create(&args,&trial_timer);if(e!=ESP_OK)return e;
    return esp_timer_start_once(trial_timer,90000000);
}
esp_err_t gw_ota_execute(const cJSON *command_json,uint64_t deadline,cJSON **details){
    const char *id=gw_action_string(command_json,"taskId");esp_err_t e=gw_core_maintenance_acquire(id);if(e!=ESP_OK)return e;
    cJSON *m=NULL,*j=NULL;e=manifest(command_json,deadline,&m);if(e!=ESP_OK)goto done;
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(command_json,"parameters");const char *component=gw_action_string(p,"component");if(!*component)component="GATEWAY";
    uint8_t boot[16],dfu[62];uint32_t caps;
    if(!gw_nrf_identity(boot,&caps,NULL)){e=ESP_ERR_INVALID_STATE;goto done;}
    if(!strcmp(component,"NRF54")&&!(caps&(1u<<12))){e=ESP_ERR_NOT_SUPPORTED;goto done;}
    if(caps&(1u<<12)){e=dfu_status(dfu,deadline);if(e!=ESP_OK||!dfu[9]||(dfu[8]!=0&&dfu[8]!=4)){if(e==ESP_OK)e=ESP_ERR_INVALID_STATE;goto done;}}
    j=cJSON_CreateObject();if(!j){e=ESP_ERR_NO_MEM;goto done;}
    cJSON_AddStringToObject(j,"taskId",id);cJSON_AddStringToObject(j,"component",component);cJSON_AddStringToObject(j,"artifactId",gw_action_string(p,"artifactId"));
    cJSON_AddStringToObject(j,"version",gw_action_string(m,"version"));cJSON_AddStringToObject(j,"sha256",gw_action_string(m,"sha256"));
    char hex[33],session_text[24];gw_action_hex_encode(boot,16,hex);cJSON_AddStringToObject(j,"previousBoot",hex);
    uint64_t session;do{esp_fill_random(&session,sizeof(session));}while(!session);snprintf(session_text,sizeof(session_text),"%llu",(unsigned long long)session);cJSON_AddStringToObject(j,"session",session_text);
    if((e=gw_ble_pause(deadline))!=ESP_OK)goto done;
    if((e=download(j,deadline))!=ESP_OK)goto done;
    if(!strcmp(component,"NRF54")){e=nrf_transfer(j,deadline,details);goto done;}
    const esp_partition_t *candidate=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,gw_action_string(j,"partition"));
    if(!candidate){e=ESP_ERR_NOT_FOUND;goto done;}
    if((e=phase(j,"REBOOT_INTENT"))!=ESP_OK)goto done;
    if((e=esp_ota_set_boot_partition(candidate))!=ESP_OK)goto done;
    esp_restart();e=ESP_ERR_NOT_FINISHED;
done:
    /* READY, transfer or reboot ambiguity owns maintenance until recovery.
     * A download failure has no active candidate and may release safely. */
    if(e==ESP_OK||(j&&(!*gw_action_string(j,"phase")||!strcmp(gw_action_string(j,"phase"),"DOWNLOADING")||!strcmp(gw_action_string(j,"phase"),"VERIFIED_ARTIFACT")||!strcmp(gw_action_string(j,"phase"),"ROLLED_BACK")))||!j){
        if(j&&e!=ESP_OK)phase(j,"FAILED");gw_core_maintenance_release(id);(void)gw_ble_restore(gw_core_mono_ms()+2000);
    }else e=ESP_ERR_NOT_FINISHED;
    cJSON_Delete(j);cJSON_Delete(m);return e;
}
esp_err_t gw_ota_recover(const cJSON *command_json,uint64_t deadline,cJSON **details){
    cJSON *j=gw_store_get_json(OTA_KEY);const char *id=gw_action_string(command_json,"taskId");esp_err_t e=ESP_ERR_TIMEOUT;
    if(!j||strcmp(gw_action_string(j,"taskId"),id))goto done;
    if(!strcmp(gw_action_string(j,"phase"),"CONFIRMED")){uint8_t boot[16];if(!strcmp(gw_action_string(j,"component"),"NRF54")&&!gw_action_hex(gw_action_string(j,"confirmedBoot"),boot,16))goto done;*details=result(j,!strcmp(gw_action_string(j,"component"),"NRF54")?boot:NULL);e=ESP_OK;goto done;}
    if(!strcmp(gw_action_string(j,"phase"),"FAILED")||!strcmp(gw_action_string(j,"phase"),"ROLLED_BACK")){e=ESP_FAIL;goto done;}
    if((e=gw_core_maintenance_acquire(id))!=ESP_OK)goto done;
    if(!strcmp(gw_action_string(j,"component"),"NRF54")){e=nrf_transfer(j,deadline,details);goto done;}
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(strcmp(running->label,gw_action_string(j,"partition"))||strcmp(esp_app_get_description()->version,gw_action_string(j,"version"))){phase(j,"ROLLED_BACK");e=ESP_ERR_INVALID_VERSION;goto done;}
    uint8_t hash[32],expected[32];const cJSON *size=cJSON_GetObjectItemCaseSensitive(j,"size");
    if(!cJSON_IsNumber(size)||size->valuedouble<32||partition_hash(running,(size_t)size->valuedouble,hash)!=ESP_OK||!gw_action_hex(gw_action_string(j,"sha256"),expected,32)||memcmp(hash,expected,32)){e=ESP_ERR_INVALID_CRC;goto done;}
    if(trial_deadline&&deadline>trial_deadline-100)deadline=trial_deadline-100;
    if(!gw_core_time_trusted()||(!gw_core_interface_ready("WIFI_STA")&&!gw_core_interface_ready("ETHERNET"))||nrf_health(deadline)!=ESP_OK){e=ESP_ERR_NOT_FINISHED;goto done;}
    uint8_t dfu[62];uint32_t caps=0;gw_nrf_identity(NULL,&caps,NULL);if((caps&(1u<<12))&&(dfu_status(dfu,deadline)!=ESP_OK||!dfu[9])){e=ESP_ERR_NOT_FINISHED;goto done;}
    if((e=phase(j,"CONFIRMING"))!=ESP_OK)goto done;
    e=esp_ota_mark_app_valid_cancel_rollback();if(e!=ESP_OK)goto done;
    if(trial_timer)esp_timer_stop(trial_timer);trial_deadline=0;
    e=phase(j,"CONFIRMED");if(e==ESP_OK){*details=result(j,NULL);if(!*details)e=ESP_ERR_NO_MEM;}
done:
    if(e==ESP_OK||(j&&!strcmp(gw_action_string(j,"phase"),"ROLLED_BACK"))){gw_core_maintenance_release(id);(void)gw_ble_restore(gw_core_mono_ms()+2000);}
    cJSON_Delete(j);return e;
}
