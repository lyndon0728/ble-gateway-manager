#include "gw_signed_time.h"
#include "monocypher-ed25519.h"
#include "mbedtls/base64.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *nonce="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
static const char *boot="0102030405060708090a0b0c0d0e0f10";
static uint8_t secret[64],pub[32];
static cJSON *response(const char *raw) {
    uint8_t msg[1037],sig[64];memcpy(msg,"gw32-time-v1",12);memcpy(msg+12,raw,strlen(raw));
    crypto_ed25519_sign(sig,secret,msg,12+strlen(raw));
    unsigned char payload[1400],signature[90];size_t n;
    assert(!mbedtls_base64_encode(payload,sizeof(payload),&n,(uint8_t *)raw,strlen(raw)));
    assert(!mbedtls_base64_encode(signature,sizeof(signature),&n,sig,64));
    cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"keyId","clock-1");
    cJSON_AddStringToObject(r,"payloadBase64",(char *)payload);cJSON_AddStringToObject(r,"signatureBase64",(char *)signature);return r;
}
static esp_err_t verify(cJSON *r){uint64_t issued;uint32_t uncertainty;return gw_signed_time_verify(r,"clock-1",pub,nonce,boot,&issued,&uncertainty);}
int main(void){
    uint8_t seed[32]={1};crypto_ed25519_key_pair(secret,pub,seed);
    const char *raw="{\"schemaVersion\":\"1.0\",\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"bootId\":\"0102030405060708090a0b0c0d0e0f10\",\"issuedAtUnixMs\":\"1800000000000\",\"uncertaintyMs\":1000,\"validForMs\":60000}";
    cJSON *r=response(raw);assert(verify(r)==ESP_OK);
    uint64_t issued;uint32_t uncertainty;
    assert(gw_signed_time_verify(r,"clock-1",pub,"fresh-nonce",boot,&issued,&uncertainty)!=ESP_OK);
    assert(gw_signed_time_verify(r,"clock-1",pub,nonce,"fresh-boot",&issued,&uncertainty)!=ESP_OK);
    assert(gw_signed_time_verify(r,"wrong-key",pub,nonce,boot,&issued,&uncertainty)!=ESP_OK);
    cJSON_GetObjectItemCaseSensitive(r,"signatureBase64")->valuestring[0]^=1;
    assert(verify(r)!=ESP_OK);cJSON_Delete(r);
    r=response(raw);cJSON_AddStringToObject(r,"keyId","clock-1");assert(verify(r)!=ESP_OK);cJSON_Delete(r);
    char scientific[1024];const char *at=strstr(raw,"1000");
    snprintf(scientific,sizeof(scientific),"%.*s1e3%s",(int)(at-raw),raw,at+4);
    r=response(scientific);assert(verify(r)!=ESP_OK);cJSON_Delete(r);
    const char *bad[]={"\"issuedAtUnixMs\":\"01800000000000\"","\"issuedAtUnixMs\":\"18446744073709551616\"","\"uncertaintyMs\":1000.1","\"validForMs\":60001","\"unexpected\":true","\"bootId\":\"ffffffffffffffffffffffffffffffff\"","\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAE=\""};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){
        /* Duplicate canonical fields and unknown fields must all be rejected,
         * even with a valid signature over their exact bytes. */
        char tampered[1024];snprintf(tampered,sizeof(tampered),"%.*s,%s}",(int)strlen(raw)-1,raw,bad[i]);
        r=response(tampered);assert(verify(r)!=ESP_OK);cJSON_Delete(r);
    }
    puts("PASS: signed time exact-byte Ed25519, schema, duplicate, nonce/boot tampering");
}
