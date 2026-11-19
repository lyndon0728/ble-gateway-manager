#include "gw_internal.h"
#include "gw_rules.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_random.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "freertos/task.h"

static SemaphoreHandle_t core_lock;
static SemaphoreHandle_t credential_lock;
static cJSON *state;
static gw_network_probe_fn probe_handler;
static void *probe_ctx;
static uint64_t window_deadline;
static char maintenance[129];
static bool work_requested,core_fault;
static bool background_probe;
static uint64_t next_background_probe;
static atomic_bool cancel_requested;
static const char *current_request_id;
static uint8_t request_key[32];

static void replace(cJSON *o,const char *key,const cJSON *v)
{cJSON_DeleteItemFromObjectCaseSensitive(o,key);if(v)cJSON_AddItemToObject(o,key,cJSON_Duplicate(v,true));else cJSON_AddNullToObject(o,key);}
static bool field_u64(const cJSON *o,const char *key,uint64_t *v){return gw_json_u64(cJSON_GetObjectItemCaseSensitive(o,key),v);}
static esp_err_t save(void)
{
    esp_err_t e=gw_store_put_json("core/state",state);
    if(e!=ESP_OK){core_fault=true;gw_json_set_string(state,"verificationState","STORAGE_FAULT");cJSON *t=cJSON_GetObjectItemCaseSensitive(state,"transaction");if(t)gw_json_set_string(t,"state","FAULT");}
    return e;
}
static cJSON *transaction(void){return cJSON_GetObjectItemCaseSensitive(state,"transaction");}
static bool busy_tx(void)
{const char *s=gw_json_string(transaction(),"state");return s&&(!strcmp(s,"DRAFT")||!strcmp(s,"PROBING"));}
static bool network_busy(void){return busy_tx()||background_probe||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"remoteAckPending"));}
static cJSON *response(const char *id,const char *code,cJSON *body)
{
    cJSON *r=cJSON_CreateObject();if(!r){cJSON_Delete(body);return NULL;}
    cJSON_AddStringToObject(r,"apiVersion","1.0");cJSON_AddStringToObject(r,"requestId",id?id:"");cJSON_AddStringToObject(r,"code",code);
    cJSON_AddItemToObject(r,"body",body?body:cJSON_CreateObject());return r;
}
static const char *code_for(esp_err_t err)
{
    switch(err){case ESP_OK:return "OK";case ESP_ERR_NO_MEM:return "STORAGE_FULL";case ESP_ERR_NOT_FOUND:return "CREDENTIAL_NOT_INSTALLED";
    case ESP_ERR_NOT_SUPPORTED:return "UNSUPPORTED";case ESP_ERR_INVALID_ARG:return "INVALID_ARGUMENT";case ESP_ERR_TIMEOUT:return "PROBE_FAILED";default:return "STORAGE_FAULT";}
}
static void random_id(char out[65])
{uint8_t raw[16];esp_fill_random(raw,sizeof(raw));for(unsigned i=0;i<16;i++)snprintf(out+i*2,3,"%02x",raw[i]);out[32]=0;}
static void credential_key(const char *ref,char out[80])
{
    unsigned char hash[32];mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),(const unsigned char*)ref,strlen(ref),hash);
    for(unsigned i=0;i<8;i++)snprintf(out+2*i,3,"%02x",hash[i]);out[15]=0;
}
static cJSON *load_credential(const char *ref)
{
    char key[80];credential_key(ref,key);size_t n=0;if(gw_secret_get(key,NULL,&n)!=ESP_OK||n>4096)return NULL;
    char *raw=malloc(n+1);if(!raw)return NULL;
    if(gw_secret_get(key,raw,&n)!=ESP_OK){free(raw);return NULL;}raw[n]=0;cJSON *v=cJSON_Parse(raw);memset(raw,0,n);free(raw);return v;
}
static bool credential_published(const char *ref)
{
    cJSON *index=gw_store_get_json("credential/index");bool found=false;
    for(const cJSON *v=index?index->child:NULL;v;v=v->next){const char *r=gw_json_string(v,"credentialRef");if(r&&!strcmp(r,ref)){found=true;break;}}
    cJSON_Delete(index);return found;
}
esp_err_t gw_credential_password(const char *ref,const char *ssid,const char *security,char password[65])
{
    if(!ref||!ssid||!security)return ESP_ERR_INVALID_ARG;
    if(!credential_published(ref))return ESP_ERR_NOT_FOUND;
    cJSON *cred=load_credential(ref);if(!cred)return ESP_ERR_NOT_FOUND;
    const char *r=gw_json_string(cred,"credentialRef"),*s=gw_json_string(cred,"ssid"),*mode=gw_json_string(cred,"security"),*pass=gw_json_string(cred,"password");
    esp_err_t e=ESP_ERR_NOT_FOUND;
    if(r&&s&&mode&&pass&&!strcmp(ref,r)&&!strcmp(ssid,s)&&!strcmp(security,mode)&&strlen(pass)>=8&&strlen(pass)<=63){strcpy(password,pass);e=ESP_OK;}
    if(pass)memset((char*)pass,0,strlen(pass));cJSON_Delete(cred);return e;
}
static esp_err_t install_credential(const char *ref,const char *version,const char *ssid,const char *security,const char *password,const char *delivery_id)
{
    if(!ref||!*ref||strlen(ref)>128||!version||strlen(version)>64||!ssid||!*ssid||strlen(ssid)>32||!security||
       (strcmp(security,"WPA2_PSK")&&strcmp(security,"WPA3_SAE"))||!password||strlen(password)<8||strlen(password)>63||!delivery_id||strlen(delivery_id)>128)return ESP_ERR_INVALID_ARG;
    char key[80];credential_key(ref,key);cJSON *old=load_credential(ref);bool existing=false;
    if(!old){size_t existing_size=0;esp_err_t stored=gw_secret_get(key,NULL,&existing_size);if(stored==ESP_OK||credential_published(ref))return ESP_ERR_INVALID_STATE;if(stored!=ESP_ERR_NVS_NOT_FOUND)return stored;}
    if(old){const char *r=gw_json_string(old,"credentialRef"),*v=gw_json_string(old,"version"),*s=gw_json_string(old,"ssid"),*p=gw_json_string(old,"password"),*sec=gw_json_string(old,"security");bool same=r&&v&&s&&p&&sec&&!strcmp(r,ref)&&!strcmp(v,version)&&!strcmp(s,ssid)&&!strcmp(p,password)&&!strcmp(sec,security);if(p)memset((char*)p,0,strlen(p));cJSON_Delete(old);if(!same)return ESP_ERR_INVALID_STATE;existing=true;if(credential_published(ref))return ESP_OK;}
    cJSON *all=gw_store_get_json("credential/index");if(!all)all=cJSON_CreateArray();
    if(!all)return ESP_ERR_NO_MEM;
    if(cJSON_GetArraySize(all)>=16){cJSON_Delete(all);return ESP_ERR_NO_MEM;}
    cJSON *cred=cJSON_CreateObject(),*meta=cJSON_CreateObject();if(!cred||!meta){cJSON_Delete(cred);cJSON_Delete(meta);cJSON_Delete(all);return ESP_ERR_NO_MEM;}
    cJSON_AddStringToObject(cred,"credentialRef",ref);cJSON_AddStringToObject(cred,"version",version);cJSON_AddStringToObject(cred,"ssid",ssid);cJSON_AddStringToObject(cred,"security",security);cJSON_AddStringToObject(cred,"password",password);cJSON_AddStringToObject(cred,"deliveryId",delivery_id);
    cJSON_AddStringToObject(meta,"credentialRef",ref);cJSON_AddStringToObject(meta,"version",version);cJSON_AddStringToObject(meta,"ssid",ssid);cJSON_AddStringToObject(meta,"security",security);cJSON_AddStringToObject(meta,"state","INSTALLED");cJSON_AddStringToObject(meta,"source",*delivery_id?"PLATFORM":"LOCAL");cJSON_AddItemToArray(all,meta);
    char *a=cJSON_PrintUnformatted(cred),*b=cJSON_PrintUnformatted(all);esp_err_t err=ESP_ERR_NO_MEM;
    if(a&&b){err=existing?ESP_OK:gw_secret_put(key,a,strlen(a));if(err==ESP_OK)err=gw_store_put("credential/index",b,strlen(b));}
    if(a){memset(a,0,strlen(a));free(a);}free(b);char *secret=(char*)gw_json_string(cred,"password");if(secret)memset(secret,0,strlen(secret));cJSON_Delete(cred);cJSON_Delete(all);return err;
}
esp_err_t gw_core_credential_install(const char *ref,const char *version,const char *ssid,const char *security,const char *password,const char *delivery_id)
{
    if(!credential_lock)return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(credential_lock,portMAX_DELAY);
    esp_err_t err=install_credential(ref,version,ssid,security,password,delivery_id);
    xSemaphoreGive(credential_lock);return err;
}
cJSON *gw_core_credential_metadata(void)
{cJSON *all=gw_store_get_json("credential/index"),*local=cJSON_CreateArray();if(!local){cJSON_Delete(all);return NULL;}for(const cJSON *v=all?all->child:NULL;v;v=v->next){const char *source=gw_json_string(v,"source");if(source&&!strcmp(source,"LOCAL"))cJSON_AddItemToArray(local,cJSON_Duplicate(v,true));}cJSON_Delete(all);return local;}

static cJSON *public_state(void)
{
    cJSON *out=cJSON_CreateObject();if(!out)return NULL;
    const char *keys[]={"networkGeneration","localRevision","desiredLocalNetwork","actualNetwork","localOverride","verificationState","lastKnownPlatformRevision","appliedTaskId"};
    for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);i++)replace(out,keys[i],cJSON_GetObjectItemCaseSensitive(state,keys[i]));
    const char *iface="NONE";const cJSON *actual=cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"),*order=cJSON_GetObjectItemCaseSensitive(actual,"uplinkOrder");
    for(const cJSON *u=order?order->child:NULL;u;u=u->next)if(cJSON_IsString(u)&&gw_core_interface_ready(u->valuestring)){iface=u->valuestring;break;}
    cJSON_AddStringToObject(out,"activeInterface",iface);cJSON_AddItemToObject(out,"credentials",gw_core_credential_metadata());
    cJSON_AddBoolToObject(out,"timeTrusted",gw_core_time_trusted());
    cJSON_AddNumberToObject(out,"windowRemainingMs",window_deadline>gw_core_mono_ms()?(double)(window_deadline-gw_core_mono_ms()):0);
    replace(out,"transaction",transaction());return out;
}
cJSON *gw_core_state(void){if(!state)return NULL;xSemaphoreTake(core_lock,portMAX_DELAY);cJSON *r=public_state();xSemaphoreGive(core_lock);return r;}
cJSON *gw_core_transaction(const char *id)
{
    if(!state||!id)return NULL;xSemaphoreTake(core_lock,portMAX_DELAY);cJSON *t=transaction(),*result=NULL;
    const char *v=gw_json_string(t,"transactionId");if(v&&!strcmp(v,id))result=cJSON_Duplicate(t,true);
    if(!result){const cJSON *h=cJSON_GetObjectItemCaseSensitive(state,"history");for(const cJSON *p=h?h->child:NULL;p;p=p->next){v=gw_json_string(p,"transactionId");if(v&&!strcmp(v,id)){result=cJSON_Duplicate(p,true);break;}}}
    xSemaphoreGive(core_lock);return result;
}
cJSON *gw_core_transaction_by_task(const char *id)
{
    if(!state||!id)return NULL;xSemaphoreTake(core_lock,portMAX_DELAY);cJSON *t=transaction(),*result=NULL;const char *v=gw_json_string(t,"taskId");if(v&&!strcmp(v,id))result=cJSON_Duplicate(t,true);
    if(!result){const cJSON *h=cJSON_GetObjectItemCaseSensitive(state,"history");for(const cJSON *p=h?h->child:NULL;p;p=p->next){v=gw_json_string(p,"taskId");if(v&&!strcmp(v,id)){result=cJSON_Duplicate(p,true);break;}}}
    xSemaphoreGive(core_lock);return result;
}
static esp_err_t begin(const cJSON *network,bool remote,const char *task_id,const char *revision,uint64_t deadline,cJSON **out)
{
    if(network_busy()||*maintenance)return ESP_ERR_INVALID_STATE;
    char error[80];esp_err_t e=gw_network_validate(network,error,sizeof(error));if(e!=ESP_OK)return e;
    cJSON *history=cJSON_GetObjectItemCaseSensitive(state,"history");if(!history){history=cJSON_CreateArray();cJSON_AddItemToObject(state,"history",history);}
    if(transaction()){
        /* Bounded history; old terminal transactions remain protected by the
         * caller's durable task/request idempotency journal. */
        if(cJSON_GetArraySize(history)>=8)cJSON_DeleteItemFromArray(history,0);
        cJSON_AddItemToArray(history,cJSON_Duplicate(transaction(),true));
    }
    cJSON *t=cJSON_CreateObject();if(!t)return ESP_ERR_NO_MEM;char id[65],hash[65];random_id(id);
    e=gw_core_network_snapshot_id(network,hash);if(e!=ESP_OK){cJSON_Delete(t);return e;}
    cJSON_AddStringToObject(t,"transactionId",id);cJSON_AddStringToObject(t,"source",remote?"REMOTE":"LOCAL");cJSON_AddStringToObject(t,"taskId",task_id?task_id:"");cJSON_AddStringToObject(t,"desiredRevision",revision?revision:"0");
    cJSON_AddStringToObject(t,"state","DRAFT");cJSON_AddStringToObject(t,"code","OK");cJSON_AddStringToObject(t,"snapshotId",hash);gw_json_set_u64(t,"deadlineMonoMs",deadline);
    if(current_request_id)cJSON_AddStringToObject(t,"createRequestId",current_request_id);
    replace(t,"network",network);replace(t,"before",cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"));replace(t,"baseGeneration",cJSON_GetObjectItemCaseSensitive(state,"networkGeneration"));
    replace(state,"transaction",t);cJSON_Delete(t);cancel_requested=false;e=save();
    if(e==ESP_OK&&out)*out=cJSON_Duplicate(transaction(),true);return e;
}
static esp_err_t commit(bool local_only)
{
    cJSON *t=transaction();uint64_t generation,base,local;
    if(!field_u64(state,"networkGeneration",&generation)||!field_u64(t,"baseGeneration",&base)||generation!=base||generation==UINT64_MAX||!field_u64(state,"localRevision",&local)||local==UINT64_MAX)return ESP_ERR_INVALID_STATE;
    bool remote=!strcmp(gw_json_string(t,"source"),"REMOTE");const cJSON *network=cJSON_GetObjectItemCaseSensitive(t,"network");
    replace(state,"previous",cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"));replace(state,"actualNetwork",network);replace(state,"desiredLocalNetwork",network);
    if(!local_only)replace(state,"lastVerified",network);
    gw_json_set_u64(state,"networkGeneration",generation+1);if(!remote)gw_json_set_u64(state,"localRevision",local+1);
    cJSON_DeleteItemFromObjectCaseSensitive(state,"localOverride");cJSON_AddBoolToObject(state,"localOverride",!remote);
    gw_json_set_string(state,"verificationState",local_only?"LOCAL_ONLY":"PLATFORM_CONNECTED");
    cJSON_DeleteItemFromObjectCaseSensitive(state,"localAttemptActive");cJSON_AddBoolToObject(state,"localAttemptActive",local_only);
    if(local_only)gw_json_set_u64(state,"localAttemptDeadline",gw_core_mono_ms()+120000);
    if(remote){uint64_t rev=0;if(!field_u64(t,"desiredRevision",&rev)||rev>9007199254740991ULL)return ESP_ERR_INVALID_ARG;gw_json_set_u64(state,"pendingPlatformRevision",rev);gw_json_set_string(state,"appliedTaskId",gw_json_string(t,"taskId"));cJSON_DeleteItemFromObjectCaseSensitive(state,"remoteAckPending");cJSON_AddBoolToObject(state,"remoteAckPending",true);}
    gw_json_set_string(t,"state","COMMITTED");gw_json_set_string(t,"code",local_only?"LOCAL_ONLY":"OK");return save();
}
static void finish_failed(const char *reason)
{
    cJSON *t=transaction();replace(state,"actualNetwork",cJSON_GetObjectItemCaseSensitive(t,"before"));gw_json_set_string(t,"state",cancel_requested?"CANCELLED":"ROLLED_BACK");gw_json_set_string(t,"code",reason);save();
}
static void local_background(void)
{
    xSemaphoreTake(core_lock,portMAX_DELAY);
    const char *verification=gw_json_string(state,"verificationState");
    if(core_fault||network_busy()||*maintenance||!verification||strcmp(verification,"LOCAL_ONLY")||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"localAttemptActive"))){xSemaphoreGive(core_lock);return;}
    uint64_t deadline=0,generation=0;field_u64(state,"localAttemptDeadline",&deadline);field_u64(state,"networkGeneration",&generation);
    if(gw_core_mono_ms()>=deadline){
        const cJSON *last=cJSON_GetObjectItemCaseSensitive(state,"lastVerified");cJSON *fallback=cJSON_IsObject(last)?cJSON_Duplicate(last,true):NULL;
        cJSON_DeleteItemFromObjectCaseSensitive(state,"localAttemptActive");cJSON_AddBoolToObject(state,"localAttemptActive",false);
        if(fallback)replace(state,"actualNetwork",fallback);
        esp_err_t e=save();xSemaphoreGive(core_lock);if(e==ESP_OK&&fallback)gw_network_apply(fallback);cJSON_Delete(fallback);return;
    }
    if(!probe_handler||!gw_core_time_trusted()||gw_core_mono_ms()<next_background_probe){xSemaphoreGive(core_lock);return;}
    const cJSON *network=cJSON_GetObjectItemCaseSensitive(state,"desiredLocalNetwork"),*order=cJSON_GetObjectItemCaseSensitive(network,"uplinkOrder");
    for(const cJSON *i=order?order->child:NULL;i;i=i->next)if(!gw_core_interface_ready(i->valuestring)){xSemaphoreGive(core_lock);return;}
    cJSON *job=cJSON_Duplicate(transaction(),true);if(!job){xSemaphoreGive(core_lock);return;}
    gw_network_probe_fn cb=probe_handler;void *ctx=probe_ctx;background_probe=true;xSemaphoreGive(core_lock);
    network=cJSON_GetObjectItemCaseSensitive(job,"network");order=cJSON_GetObjectItemCaseSensitive(network,"uplinkOrder");esp_err_t e=ESP_OK;
    for(const cJSON *i=order?order->child:NULL;i&&e==ESP_OK;i=i->next){
        gw_network_probe_t p={.remote=false,.network=network,.interface=i->valuestring,.deadline_mono_ms=deadline,.network_generation=generation};
        snprintf(p.transaction_id,sizeof(p.transaction_id),"%s",gw_json_string(job,"transactionId"));snprintf(p.snapshot_id,sizeof(p.snapshot_id),"%s",gw_json_string(job,"snapshotId"));strcpy(p.desired_revision,"0");
        if(gw_core_mono_ms()>=deadline)e=ESP_ERR_TIMEOUT;else e=cb(&p,ctx);
    }
    xSemaphoreTake(core_lock,portMAX_DELAY);background_probe=false;next_background_probe=gw_core_mono_ms()+5000;
    uint64_t current=0;field_u64(state,"networkGeneration",&current);char hash[65];const char *expected=gw_json_string(job,"snapshotId");
    if(e==ESP_OK&&current==generation&&gw_core_mono_ms()<deadline&&expected&&gw_core_network_snapshot_id(cJSON_GetObjectItemCaseSensitive(state,"desiredLocalNetwork"),hash)==ESP_OK&&!strcmp(hash,expected)){
        replace(state,"lastVerified",network);replace(state,"actualNetwork",network);gw_json_set_string(state,"verificationState","PLATFORM_CONNECTED");cJSON_DeleteItemFromObjectCaseSensitive(state,"localAttemptActive");cJSON_AddBoolToObject(state,"localAttemptActive",false);save();
    }
    xSemaphoreGive(core_lock);cJSON_Delete(job);
}
static void worker(void *unused)
{
    (void)unused;
    for(;;){
        vTaskDelay(pdMS_TO_TICKS(100));xSemaphoreTake(core_lock,portMAX_DELAY);
        if(!work_requested){
            cJSON *t=transaction();const char *s=gw_json_string(t,"state");uint64_t d=0;
            if(s&&!strcmp(s,"DRAFT")&&field_u64(t,"deadlineMonoMs",&d)&&gw_core_mono_ms()>=d){gw_json_set_string(t,"state","CANCELLED");gw_json_set_string(t,"code","SESSION_EXPIRED");save();}
            xSemaphoreGive(core_lock);local_background();continue;
        }
        work_requested=false;cJSON *job=cJSON_Duplicate(transaction(),true);gw_network_probe_fn cb=probe_handler;void *ctx=probe_ctx;xSemaphoreGive(core_lock);
        if(!job){xSemaphoreTake(core_lock,portMAX_DELAY);finish_failed("OUT_OF_MEMORY");xSemaphoreGive(core_lock);continue;}
        const cJSON *network=cJSON_GetObjectItemCaseSensitive(job,"network");uint64_t deadline=0;field_u64(job,"deadlineMonoMs",&deadline);
        esp_err_t err=gw_network_apply(network);const cJSON *order=cJSON_GetObjectItemCaseSensitive(network,"uplinkOrder");
        for(const cJSON *iface=order?order->child:NULL;iface&&err==ESP_OK;iface=iface->next){
            while((!gw_core_interface_ready(iface->valuestring)||!gw_core_time_trusted())&&gw_core_mono_ms()<deadline&&!cancel_requested)vTaskDelay(pdMS_TO_TICKS(100));
            if(cancel_requested||gw_core_mono_ms()>=deadline){err=ESP_ERR_TIMEOUT;break;}
            if(!gw_core_time_trusted()){err=ESP_ERR_INVALID_STATE;break;}
            gw_network_probe_t p={.remote=!strcmp(gw_json_string(job,"source"),"REMOTE"),.deadline_mono_ms=deadline,.network=network,.interface=iface->valuestring};
            snprintf(p.transaction_id,sizeof(p.transaction_id),"%s",gw_json_string(job,"transactionId"));snprintf(p.task_id,sizeof(p.task_id),"%s",gw_json_string(job,"taskId"));snprintf(p.desired_revision,sizeof(p.desired_revision),"%s",gw_json_string(job,"desiredRevision"));snprintf(p.snapshot_id,sizeof(p.snapshot_id),"%s",gw_json_string(job,"snapshotId"));field_u64(job,"baseGeneration",&p.network_generation);
            err=cb?cb(&p,ctx):ESP_ERR_NOT_SUPPORTED;
        }
        xSemaphoreTake(core_lock,portMAX_DELAY);
        if(err==ESP_OK&&!cancel_requested&&gw_core_mono_ms()<deadline)err=commit(false);
        if(err!=ESP_OK||cancel_requested){finish_failed(!gw_core_time_trusted()?"TIME_UNTRUSTED":cancel_requested?"CANCELLED":"PROBE_FAILED");}
        cJSON *actual=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"),true);xSemaphoreGive(core_lock);
        if(err!=ESP_OK||cancel_requested)gw_network_apply(cJSON_IsObject(actual)?actual:NULL);
        cJSON_Delete(actual);cJSON_Delete(job);
    }
}

esp_err_t gw_core_init(void)
{
    uint32_t flash_bytes=0;
    if(esp_flash_get_size(NULL,&flash_bytes)!=ESP_OK||flash_bytes!=8u*1024u*1024u||esp_psram_get_size()!=8u*1024u*1024u)return ESP_ERR_INVALID_STATE;
    esp_err_t e=gw_store_init();if(e!=ESP_OK)return e;
    core_lock=xSemaphoreCreateMutex();credential_lock=xSemaphoreCreateMutex();if(!core_lock||!credential_lock)return ESP_ERR_NO_MEM;
    e=gw_network_init();if(e!=ESP_OK)return e;
    size_t size=sizeof(request_key);e=gw_store_get("core/request-key",request_key,&size);
    if(e==ESP_ERR_NOT_FOUND){esp_fill_random(request_key,sizeof(request_key));e=gw_store_put("core/request-key",request_key,sizeof(request_key));}if(e!=ESP_OK||size!=sizeof(request_key))return ESP_ERR_INVALID_STATE;
    state=gw_store_get_json("core/state");if(!state){size_t n=0;if(gw_store_get("core/state",NULL,&n)!=ESP_ERR_NOT_FOUND)return ESP_ERR_INVALID_STATE;state=cJSON_CreateObject();if(!state)return ESP_ERR_NO_MEM;cJSON_AddNumberToObject(state,"schemaVersion",1);gw_json_set_u64(state,"networkGeneration",0);gw_json_set_u64(state,"localRevision",0);cJSON_AddBoolToObject(state,"localOverride",false);cJSON_AddStringToObject(state,"verificationState","UNCONFIGURED");cJSON_AddNumberToObject(state,"lastKnownPlatformRevision",0);cJSON_AddNullToObject(state,"actualNetwork");cJSON_AddNullToObject(state,"desiredLocalNetwork");cJSON_AddArrayToObject(state,"history");cJSON_AddArrayToObject(state,"requests");if(save()!=ESP_OK)return ESP_FAIL;}
    uint64_t generation,local;
    const cJSON *schema=cJSON_GetObjectItemCaseSensitive(state,"schemaVersion");
    if(!cJSON_IsNumber(schema)||schema->valuedouble!=1||!field_u64(state,"networkGeneration",&generation)||!field_u64(state,"localRevision",&local))return ESP_ERR_INVALID_STATE;
    if(busy_tx()){replace(state,"actualNetwork",cJSON_GetObjectItemCaseSensitive(transaction(),"before"));gw_json_set_string(transaction(),"state","ROLLED_BACK");gw_json_set_string(transaction(),"code","RESTARTED_BEFORE_COMMIT");if(save()!=ESP_OK)return ESP_FAIL;}
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"localAttemptActive"))){gw_json_set_u64(state,"localAttemptDeadline",gw_core_mono_ms()+120000);if(save()!=ESP_OK)return ESP_FAIL;}
    const cJSON *actual=cJSON_GetObjectItemCaseSensitive(state,"actualNetwork");if(cJSON_IsObject(actual)){e=gw_network_apply(actual);if(e!=ESP_OK)return e;}
    return xTaskCreate(worker,"gw_net_owner",12288,NULL,5,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
void gw_core_set_probe_handler(gw_network_probe_fn cb,void *ctx){xSemaphoreTake(core_lock,portMAX_DELAY);probe_handler=cb;probe_ctx=ctx;xSemaphoreGive(core_lock);}
void gw_core_set_local_window_deadline(uint64_t deadline)
{
    if(!state)return;xSemaphoreTake(core_lock,portMAX_DELAY);window_deadline=deadline;
    if(!deadline&&busy_tx()&&!strcmp(gw_json_string(transaction(),"source"),"LOCAL")){
        cancel_requested=true;if(!strcmp(gw_json_string(transaction(),"state"),"DRAFT")){gw_json_set_string(transaction(),"state","CANCELLED");gw_json_set_string(transaction(),"code","SESSION_EXPIRED");save();}
    }xSemaphoreGive(core_lock);
}
esp_err_t gw_core_maintenance_acquire(const char *owner)
{
    if(!owner||!*owner||strlen(owner)>=sizeof(maintenance))return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(core_lock,portMAX_DELAY);esp_err_t err=ESP_OK;if(network_busy()||(*maintenance&&strcmp(maintenance,owner)))err=ESP_ERR_INVALID_STATE;else strcpy(maintenance,owner);xSemaphoreGive(core_lock);return err;
}
void gw_core_maintenance_release(const char *owner)
{xSemaphoreTake(core_lock,portMAX_DELAY);if(owner&&!strcmp(owner,maintenance))*maintenance=0;xSemaphoreGive(core_lock);}

esp_err_t gw_core_platform_ack(const char *task_id)
{
    if(!state||!task_id)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(core_lock,portMAX_DELAY);const char *last=gw_json_string(state,"platformAckTaskId");
    if(last&&!strcmp(last,task_id)){xSemaphoreGive(core_lock);return ESP_OK;}
    const char *applied=gw_json_string(state,"appliedTaskId");uint64_t revision=0;
    if(core_fault||!applied||strcmp(applied,task_id)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"remoteAckPending"))||!field_u64(state,"pendingPlatformRevision",&revision)||revision>9007199254740991ULL){xSemaphoreGive(core_lock);return ESP_ERR_INVALID_STATE;}
    cJSON_DeleteItemFromObjectCaseSensitive(state,"lastKnownPlatformRevision");cJSON_AddNumberToObject(state,"lastKnownPlatformRevision",(double)revision);
    cJSON_DeleteItemFromObjectCaseSensitive(state,"remoteAckPending");cJSON_AddBoolToObject(state,"remoteAckPending",false);gw_json_set_string(state,"verificationState","PLATFORM_SYNCED");gw_json_set_string(state,"platformAckTaskId",task_id);
    esp_err_t err=save();xSemaphoreGive(core_lock);return err;
}

esp_err_t gw_core_remote_begin(const char *task_id,const cJSON *network,const char *revision,const cJSON *precondition,uint64_t deadline,cJSON **out)
{
    if(!state||!task_id||!*task_id||strlen(task_id)>128||!revision||strlen(revision)>23||!out)return ESP_ERR_INVALID_ARG;
    uint64_t parsed_revision;if(!gw_rules_u64(revision,&parsed_revision)||!parsed_revision||parsed_revision>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    *out=NULL;cJSON *prior=gw_core_transaction_by_task(task_id);if(prior){char hash[65];const char *old=gw_json_string(prior,"snapshotId"),*old_revision=gw_json_string(prior,"desiredRevision");if(gw_core_network_snapshot_id(network,hash)!=ESP_OK||!old||strcmp(old,hash)||!old_revision||strcmp(old_revision,revision)){cJSON_Delete(prior);return ESP_ERR_INVALID_STATE;}*out=prior;return ESP_OK;}
    xSemaphoreTake(core_lock,portMAX_DELAY);uint64_t generation=0,expected=0;esp_err_t e=ESP_OK;
    if(core_fault||!field_u64(state,"networkGeneration",&generation))e=ESP_ERR_INVALID_STATE;
    if(precondition&&(!field_u64(precondition,"expectedNetworkGeneration",&expected)||generation!=expected))e=ESP_ERR_INVALID_STATE;
    if(precondition){static const char *keys[]={"expectedNetworkGeneration","snapshotId","resolutionToken"};char hash[65];const char *supplied=gw_json_string(precondition,"snapshotId");if(!gw_json_allowed(precondition,keys,3)||!supplied||gw_core_network_snapshot_id(cJSON_GetObjectItemCaseSensitive(state,"desiredLocalNetwork"),hash)!=ESP_OK||strcmp(supplied,hash))e=ESP_ERR_INVALID_STATE;}
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"localOverride"))){
        char hash[65];const char *supplied=gw_json_string(precondition,"snapshotId"),*resolution=gw_json_string(precondition,"resolutionToken");
        if(!supplied||!resolution||!*resolution||gw_core_network_snapshot_id(cJSON_GetObjectItemCaseSensitive(state,"desiredLocalNetwork"),hash)!=ESP_OK||strcmp(supplied,hash))e=ESP_ERR_INVALID_STATE;
    }
    uint64_t max=gw_core_mono_ms()+120000;if(deadline>max)deadline=max;if(deadline<=gw_core_mono_ms())e=ESP_ERR_TIMEOUT;
    if(e==ESP_OK)e=begin(network,true,task_id,revision,deadline,out);
    if(e==ESP_OK){gw_json_set_string(transaction(),"state","PROBING");e=save();if(e==ESP_OK)work_requested=true;cJSON_Delete(*out);*out=cJSON_Duplicate(transaction(),true);}
    xSemaphoreGive(core_lock);return e;
}

static bool versions_match(const cJSON *req)
{uint64_t a,b,c,d;return field_u64(req,"expectedNetworkGeneration",&a)&&field_u64(req,"baseLocalRevision",&b)&&field_u64(state,"networkGeneration",&c)&&field_u64(state,"localRevision",&d)&&a==c&&b==d;}
static esp_err_t request_digest(const cJSON *req,char out[65])
{char *text=gw_json_canonical(req);uint8_t hash[32]={0};if(!text)return ESP_ERR_NO_MEM;int rc=mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),request_key,sizeof(request_key),(const unsigned char*)text,strlen(text),hash);memset(text,0,strlen(text));free(text);if(rc)return ESP_FAIL;for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",hash[i]);return ESP_OK;}

static bool valid_body(const char *op,const cJSON *body)
{
    const char *credential[]={"ssid","security","password"},*network[]={"network"},*tx[]={"transactionId"},*save_keys[]={"transactionId","acknowledgeUnverified"},*scan[]={"mode"};
    if(!strcmp(op,"PUT_CREDENTIAL"))return gw_json_allowed(body,credential,3);
    if(!strcmp(op,"CREATE_CANDIDATE")||!strcmp(op,"VALIDATE_NETWORK"))return gw_json_allowed(body,network,1);
    if(!strcmp(op,"GET_TRANSACTION")||!strcmp(op,"APPLY_ONLINE")||!strcmp(op,"CANCEL_TRANSACTION"))return gw_json_allowed(body,tx,1);
    if(!strcmp(op,"SAVE_LOCAL_ONLY"))return gw_json_allowed(body,save_keys,2);
    if(!strcmp(op,"SCAN_WIFI"))return gw_json_allowed(body,scan,1);
    return cJSON_IsObject(body)&&!body->child;
}
static cJSON *recover_request(const char *id,const char *op,const char *digest)
{
    cJSON *t=transaction();const char *create=gw_json_string(t,"createRequestId"),*action=gw_json_string(t,"actionRequestId");
    if((create&&!strcmp(create,id))||(action&&!strcmp(action,id)))return response(id,"OK",cJSON_Duplicate(t,true));
    const cJSON *history=cJSON_GetObjectItemCaseSensitive(state,"history");
    for(const cJSON *p=history?history->child:NULL;p;p=p->next){create=gw_json_string(p,"createRequestId");action=gw_json_string(p,"actionRequestId");if((create&&!strcmp(create,id))||(action&&!strcmp(action,id)))return response(id,"OK",cJSON_Duplicate(p,true));}
    if(!strcmp(op,"PUT_CREDENTIAL")){
        char ref[65];snprintf(ref,sizeof(ref),"local-%.32s",digest);
        if(credential_published(ref)){cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"credentialRef",ref);cJSON_AddStringToObject(r,"version","1");return response(id,"OK",r);}
    }
    return NULL; /* Intent persisted, but no durable effect: retry is safe. */
}

esp_err_t gw_core_dispatch(const cJSON *req,cJSON **out)
{
    static const char *keys[]={"apiVersion","requestId","operation","body","expectedNetworkGeneration","baseLocalRevision"};
    if(!out||!state)return ESP_ERR_INVALID_ARG;*out=NULL;
    const char *id=gw_json_string(req,"requestId"),*op=gw_json_string(req,"operation"),*version=gw_json_string(req,"apiVersion");const cJSON *body=cJSON_GetObjectItemCaseSensitive(req,"body");
    if(!gw_json_allowed(req,keys,6)||!id||!*id||strlen(id)>64||!op||!version||strcmp(version,"1.0")||!valid_body(op,body)){*out=response(id,"INVALID_ARGUMENT",NULL);return ESP_OK;}
    xSemaphoreTake(core_lock,portMAX_DELAY);
    if(!window_deadline||gw_core_mono_ms()>=window_deadline){*out=response(id,"SESSION_EXPIRED",NULL);xSemaphoreGive(core_lock);return ESP_OK;}
    if(core_fault){*out=response(id,"STORAGE_FAULT",NULL);xSemaphoreGive(core_lock);return ESP_OK;}
    bool read=!strcmp(op,"GET_STATE")||!strcmp(op,"GET_TRANSACTION")||!strcmp(op,"VALIDATE_NETWORK")||!strcmp(op,"SCAN_WIFI");char digest[65];
    if(request_digest(req,digest)!=ESP_OK){xSemaphoreGive(core_lock);return ESP_ERR_NO_MEM;}
    cJSON *requests=cJSON_GetObjectItemCaseSensitive(state,"requests"),*request_record=NULL;
    if(!requests){requests=cJSON_CreateArray();cJSON_AddItemToObject(state,"requests",requests);}
    if(!read)for(cJSON *p=requests?requests->child:NULL;p;p=p->next){const char *r=gw_json_string(p,"requestId"),*h=gw_json_string(p,"digest");if(r&&!strcmp(r,id)){
        if(!h||strcmp(h,digest))*out=response(id,"REQUEST_CONFLICT",NULL);
        else {const cJSON *cached=cJSON_GetObjectItemCaseSensitive(p,"response");*out=cached?cJSON_Duplicate(cached,true):recover_request(id,op,digest);request_record=p;}
        if(*out){xSemaphoreGive(core_lock);return ESP_OK;}break;
    }}
    if(!read&&!request_record){
        uint64_t generation=0;field_u64(state,"networkGeneration",&generation);
        for(int i=cJSON_GetArraySize(requests)-1;i>=0;i--){uint64_t old=0;if(field_u64(cJSON_GetArrayItem(requests,i),"generation",&old)&&old<generation)cJSON_DeleteItemFromArray(requests,i);}
        if(cJSON_GetArraySize(requests)>=32){*out=response(id,"STORAGE_FULL",NULL);xSemaphoreGive(core_lock);return ESP_OK;}
        request_record=cJSON_CreateObject();if(!request_record){xSemaphoreGive(core_lock);return ESP_ERR_NO_MEM;}
        cJSON_AddStringToObject(request_record,"requestId",id);cJSON_AddStringToObject(request_record,"digest",digest);gw_json_set_u64(request_record,"generation",generation);cJSON_AddItemToArray(requests,request_record);
        if(save()!=ESP_OK){*out=response(id,"STORAGE_FAULT",NULL);xSemaphoreGive(core_lock);return ESP_OK;}
    }
    current_request_id=read?NULL:id;
    const char *code="OK";cJSON *result=NULL;esp_err_t err=ESP_OK;
    if(!read&&!versions_match(req))code="REVISION_CONFLICT";
    else if(!strcmp(op,"GET_STATE"))result=public_state();
    else if(!strcmp(op,"GET_TRANSACTION")){
        const char *t=gw_json_string(body,"transactionId"),*actual=gw_json_string(transaction(),"transactionId");
        if(t&&actual&&!strcmp(t,actual))result=cJSON_Duplicate(transaction(),true);
        if(t&&!result){const cJSON *history=cJSON_GetObjectItemCaseSensitive(state,"history");for(const cJSON *p=history?history->child:NULL;p;p=p->next){const char *saved=gw_json_string(p,"transactionId");if(saved&&!strcmp(saved,t)){result=cJSON_Duplicate(p,true);break;}}}
        if(!result)code="NOT_FOUND";
    }
    else if(!strcmp(op,"SCAN_WIFI")){
        const char *mode=gw_json_string(body,"mode");if(!mode)mode="PASSIVE";
        if(strcmp(mode,"PASSIVE")&&strcmp(mode,"ACTIVE"))code="INVALID_ARGUMENT";
        else if(network_busy()||*maintenance)code="NETWORK_BUSY";
        else {background_probe=true;xSemaphoreGive(core_lock);err=gw_network_scan(!strcmp(mode,"ACTIVE"),&result);xSemaphoreTake(core_lock,portMAX_DELAY);background_probe=false;}
    }
    else if(!strcmp(op,"PUT_CREDENTIAL")){
        const char *ssid=gw_json_string(body,"ssid"),*security=gw_json_string(body,"security"),*password=gw_json_string(body,"password");char ref[65];snprintf(ref,sizeof(ref),"local-%.32s",digest);
        err=gw_core_credential_install(ref,"1",ssid,security,password,"");if(err==ESP_OK){result=cJSON_CreateObject();cJSON_AddStringToObject(result,"credentialRef",ref);cJSON_AddStringToObject(result,"version","1");}
    }
    else if(!strcmp(op,"VALIDATE_NETWORK")){char field[80]={0};err=gw_network_validate(cJSON_GetObjectItemCaseSensitive(body,"network"),field,sizeof(field));result=cJSON_CreateObject();cJSON_AddStringToObject(result,"field",field);}
    else if(!strcmp(op,"CREATE_CANDIDATE")||!strcmp(op,"RESTORE_PREVIOUS")){
        if(*maintenance)code="OTA_BUSY";else if(network_busy())code="NETWORK_BUSY";
        else err=begin(!strcmp(op,"RESTORE_PREVIOUS")?cJSON_GetObjectItemCaseSensitive(state,"previous"):cJSON_GetObjectItemCaseSensitive(body,"network"),false,NULL,NULL,window_deadline,&result);
    }
    else if(!strcmp(op,"APPLY_ONLINE")||!strcmp(op,"SAVE_LOCAL_ONLY")||!strcmp(op,"CANCEL_TRANSACTION")){
        cJSON *t=transaction();const char *txid=gw_json_string(body,"transactionId"),*actual=gw_json_string(t,"transactionId"),*phase=gw_json_string(t,"state");
        if(!txid||!actual||strcmp(txid,actual)||strcmp(gw_json_string(t,"source"),"LOCAL"))code="NOT_FOUND";
        else if(!strcmp(op,"CANCEL_TRANSACTION")){
            gw_json_set_string(t,"actionRequestId",id);
            if(busy_tx()){cancel_requested=true;if(!strcmp(phase,"DRAFT")){gw_json_set_string(t,"state","CANCELLED");gw_json_set_string(t,"code","CANCELLED");err=save();}}result=cJSON_Duplicate(t,true);
        }
        else if(!phase||strcmp(phase,"DRAFT"))code="REQUEST_CONFLICT";
        else if(!strcmp(op,"SAVE_LOCAL_ONLY")){
            if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body,"acknowledgeUnverified")))code="INVALID_ARGUMENT";
            else {gw_json_set_string(t,"actionRequestId",id);err=commit(true);if(err==ESP_OK)err=gw_network_apply(cJSON_GetObjectItemCaseSensitive(state,"actualNetwork"));result=cJSON_Duplicate(t,true);}
        }
        else {uint64_t d=gw_core_mono_ms()+120000;if(d>window_deadline)d=window_deadline;gw_json_set_string(t,"actionRequestId",id);gw_json_set_u64(t,"deadlineMonoMs",d);gw_json_set_string(t,"state","PROBING");err=save();if(err==ESP_OK)work_requested=true;result=cJSON_Duplicate(t,true);}
    }
    else if(!strcmp(op,"CLOSE_WINDOW")){window_deadline=0;if(busy_tx()&&!strcmp(gw_json_string(transaction(),"source"),"LOCAL")){cancel_requested=true;if(!strcmp(gw_json_string(transaction(),"state"),"DRAFT")){gw_json_set_string(transaction(),"state","CANCELLED");err=save();}}}
    else code="UNSUPPORTED";
    if(err!=ESP_OK)code=code_for(err);*out=response(id,code,result);
    if(!read&&*out&&request_record){
        replace(request_record,"response",*out);
        if(save()!=ESP_OK){cJSON_Delete(*out);*out=response(id,"STORAGE_FAULT",NULL);}
    }
    current_request_id=NULL;xSemaphoreGive(core_lock);return *out?ESP_OK:ESP_ERR_NO_MEM;
}
