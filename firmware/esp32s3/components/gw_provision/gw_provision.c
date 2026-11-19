// SPDX-License-Identifier: Apache-2.0
#include "gw_provision.h"
#include "gw_provision_guard.h"
#include "gw_core.h"
#include "protocomm.h"
#include "protocomm_security2.h"
#include "session.pb-c.h"
#include "sec2.pb-c.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_MAX (16384U + 16U)
#define RESPONSE_MAX 32768U
#define IDLE_MS 300000ULL
#define AUTH_FAILURE_LIMIT 5U
#define MAX_SESSION_MESSAGES 65536U

extern const uint8_t index_start[] asm("_binary_assets_index_html_start");
extern const uint8_t index_end[] asm("_binary_assets_index_html_end");
extern const uint8_t script_start[] asm("_binary_assets_app_js_start");
extern const uint8_t script_end[] asm("_binary_assets_app_js_end");
extern const uint8_t style_start[] asm("_binary_assets_style_css_start");
extern const uint8_t style_end[] asm("_binary_assets_style_css_end");

static struct {
    httpd_handle_t server;
    protocomm_t *pc;
    SemaphoreHandle_t lifecycle;
    uint64_t deadline, last_authenticated, rate_started;
    unsigned rate_count, failures, messages;
    bool open, close_requested, authenticated, session_open;
    uint32_t session_id, peer_address;
    char cookie[65], username[65], opening_revision[24];
    bool commit_grace;
    uint8_t salt[16], verifier[384];
    protocomm_security2_params_t params;
} state;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

static void secure_free(void *p, size_t len) {
    if (p) { mbedtls_platform_zeroize(p, len); free(p); }
}
static bool active(void) {
    uint64_t now = gw_core_mono_ms();
    portENTER_CRITICAL(&state_lock);
    bool result = state.open && !state.close_requested && now < state.deadline &&
                  now - state.last_authenticated < IDLE_MS;
    portEXIT_CRITICAL(&state_lock);
    return result;
}
static void touch(void) {
    portENTER_CRITICAL(&state_lock);
    state.last_authenticated = gw_core_mono_ms();
    portEXIT_CRITICAL(&state_lock);
}
static void headers(httpd_req_t *r, const char *type) {
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
}
static esp_err_t fail(httpd_req_t *r, const char *status, const char *text) {
    headers(r, "text/plain; charset=utf-8"); httpd_resp_set_status(r, status);
    return httpd_resp_send(r, text, HTTPD_RESP_USE_STRLEN);
}

/* All listeners are rejected unless their local IP is the current SoftAP IP.
 * Checking only peer subnets would incorrectly permit routed STA access. */
static bool request_allowed(httpd_req_t *r, bool post, uint32_t *peer) {
    if (!active()) return false;
    esp_netif_t *ap = gw_core_ap_netif(); esp_netif_ip_info_t ip;
    if (!ap || esp_netif_get_ip_info(ap, &ip) != ESP_OK || !ip.ip.addr) return false;
    struct sockaddr_in local = {0}, remote = {0}; socklen_t n = sizeof(local);
    int socket = httpd_req_to_sockfd(r);
    if (getsockname(socket, (struct sockaddr *)&local, &n) || local.sin_family != AF_INET || local.sin_addr.s_addr != ip.ip.addr) return false;
    n = sizeof(remote);
    if (getpeername(socket, (struct sockaddr *)&remote, &n) || remote.sin_family != AF_INET ||
        (remote.sin_addr.s_addr & ip.netmask.addr) != (ip.ip.addr & ip.netmask.addr)) return false;
    if (peer) *peer = remote.sin_addr.s_addr;
    char expected[32], host[40], origin[64], expected_origin[48];
    snprintf(expected, sizeof(expected), IPSTR, IP2STR(&ip.ip));
    if (httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK) return false;
    char with_port[40]; snprintf(with_port, sizeof(with_port), "%s:80", expected);
    if (strcmp(host, expected) && strcmp(host, with_port)) return false;
    if (post) {
        snprintf(expected_origin, sizeof(expected_origin), "http://%s", host);
        if (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK || strcmp(origin, expected_origin)) return false;
        char content_type[48];
        if (httpd_req_get_hdr_value_str(r, "Content-Type", content_type, sizeof(content_type)) != ESP_OK || strcmp(content_type, "application/octet-stream")) return false;
    }
    uint64_t now = gw_core_mono_ms();
    if (now - state.rate_started >= 1000) { state.rate_started = now; state.rate_count = 0; }
    return ++state.rate_count <= 12;
}

static esp_err_t asset(httpd_req_t *r) {
    if (!request_allowed(r, false, NULL)) return fail(r, "403 Forbidden", "Local setup interface only");
    const uint8_t *start, *end; const char *mime;
    if (!strcmp(r->uri, "/")) { start=index_start; end=index_end; mime="text/html; charset=utf-8"; }
    else if (!strcmp(r->uri, "/app.js")) { start=script_start; end=script_end; mime="application/javascript; charset=utf-8"; }
    else { start=style_start; end=style_end; mime="text/css; charset=utf-8"; }
    headers(r, mime); return httpd_resp_send(r, (const char *)start, end-start);
}
static esp_err_t info(httpd_req_t *r) {
    if (!request_allowed(r, false, NULL)) return fail(r, "403 Forbidden", "Local setup interface only");
    char text[200];
    snprintf(text, sizeof(text), "{\"apiVersion\":\"1.0\",\"securityVersion\":2,\"securityPatchVersion\":%u,\"locked\":%s}",
             protocomm_security2.patch_ver, state.failures >= AUTH_FAILURE_LIMIT ? "true" : "false");
    headers(r, "application/json"); return httpd_resp_send(r, text, HTTPD_RESP_USE_STRLEN);
}
static bool same_session(httpd_req_t *r, uint32_t peer) {
    char cookie[256];
    if (!state.session_open || peer != state.peer_address ||
        httpd_req_get_hdr_value_str(r,"Cookie",cookie,sizeof(cookie)) != ESP_OK) return false;
    bool found=false;
    for(char *part=cookie;part && *part;) {
        while(*part==' ')part++;
        char *next=strchr(part,';');if(next)*next++=0;
        if(!strncmp(part,"gw_session=",11)) {
            if(found || strlen(part+11)!=64)return false;
            unsigned difference=0;for(size_t i=0;i<64;i++)difference|=(unsigned char)part[11+i]^(unsigned char)state.cookie[i];
            if(difference)return false;found=true;
        }
        part=next;
    }
    return found;
}
static void drop_session(void) {
    if (state.session_open) protocomm_close_session(state.pc,state.session_id);
    state.session_open=false; state.authenticated=false; state.messages=0;
    mbedtls_platform_zeroize(state.cookie,sizeof(state.cookie));
}
static bool json_shape(const cJSON *j, unsigned depth) {
    if (depth > 8) return false;
    const cJSON *a;
    cJSON_ArrayForEach(a,j) {
        if (cJSON_IsObject(j)) {
            if (!a->string) return false;
            for (const cJSON *b=a->next;b;b=b->next) if (b->string && !strcmp(a->string,b->string)) return false;
        }
        if ((cJSON_IsArray(a)||cJSON_IsObject(a)) && !json_shape(a,depth+1)) return false;
    }
    return true;
}
static void wipe_json(cJSON *j) {
    if(!j)return;
    if(cJSON_IsString(j)&&j->valuestring)mbedtls_platform_zeroize(j->valuestring,strlen(j->valuestring));
    cJSON *child; cJSON_ArrayForEach(child,j)wipe_json(child);
}
static esp_err_t network_handler(uint32_t id,const uint8_t *input,ssize_t length,uint8_t **output,ssize_t *out_length,void *ctx) {
    (void)ctx;
    if (!state.authenticated || id!=state.session_id || !active() || length<2 || length>16384 || !gw_prov_payload_precheck(input,length)) return ESP_ERR_INVALID_ARG;
    char *copy=malloc((size_t)length+1); if(!copy)return ESP_ERR_NO_MEM;
    memcpy(copy,input,length);copy[length]=0;
    const char *end=NULL; cJSON *request=cJSON_ParseWithLengthOpts(copy,(size_t)length+1,&end,true);
    if(!request || !cJSON_IsObject(request) || !json_shape(request,1)) { wipe_json(request);cJSON_Delete(request);secure_free(copy,(size_t)length+1);return ESP_ERR_INVALID_ARG; }
    cJSON *response=NULL; bool close_after=false;
    const cJSON *op=cJSON_GetObjectItemCaseSensitive(request,"operation");
    esp_err_t result=gw_core_dispatch(request,&response);
    if(result==ESP_OK && response && cJSON_IsString(op) && !strcmp(op->valuestring,"CLOSE_WINDOW")) {
        const cJSON *code=cJSON_GetObjectItemCaseSensitive(response,"code");
        close_after=cJSON_IsString(code) && !strcmp(code->valuestring,"OK");
    }
    wipe_json(request);cJSON_Delete(request);secure_free(copy,(size_t)length+1);
    if(result!=ESP_OK || !response) {cJSON_Delete(response);return result==ESP_OK?ESP_FAIL:result;}
    char *encoded=cJSON_PrintUnformatted(response);cJSON_Delete(response);
    if(!encoded)return ESP_ERR_NO_MEM;
    size_t size=strlen(encoded);
    if(size>RESPONSE_MAX){secure_free(encoded,size);return ESP_ERR_INVALID_SIZE;}
    *output=(uint8_t *)encoded;*out_length=(ssize_t)size;touch();
    if(close_after)gw_provision_close_window();
    return ESP_OK;
}

static esp_err_t post(httpd_req_t *r) {
    uint32_t peer;
    if(!request_allowed(r,true,&peer))return fail(r,"403 Forbidden","Setup window, interface or request origin rejected");
    if(state.failures>=AUTH_FAILURE_LIMIT)return fail(r,"423 Locked","Setup authentication locked; authorize a new window");
    bool handshake=!strcmp(r->uri,"/prov-session");
    size_t max=handshake?2048:REQUEST_MAX;
    if(r->content_len<=0 || r->content_len>max)return fail(r,"413 Content Too Large","Request limit");
    uint8_t *input=malloc(r->content_len),*output=NULL;ssize_t output_len=0;
    if(!input)return fail(r,"503 Service Unavailable","Memory limit");
    size_t received=0;
    while(received<r->content_len){int n=httpd_req_recv(r,(char *)input+received,r->content_len-received);if(n<=0){secure_free(input,r->content_len);return ESP_FAIL;}received+=(size_t)n;}
    unsigned command=99;
    if(handshake) {
        SessionData *message=session_data__unpack(NULL,received,input);
        if(message && message->sec_ver==SEC_SCHEME_VERSION__SecScheme2 && message->proto_case==SESSION_DATA__PROTO_SEC2 && message->sec2) {
            Sec2Payload *p=message->sec2;
            if(p->msg==SEC2_MSG_TYPE__S2Session_Command0 && p->payload_case==SEC2_PAYLOAD__PAYLOAD_SC0 && p->sc0 &&
               p->sc0->client_pubkey.len==384 && p->sc0->client_username.len==strlen(state.username) &&
               !memcmp(p->sc0->client_username.data,state.username,strlen(state.username)))command=0;
            else if(p->msg==SEC2_MSG_TYPE__S2Session_Command1 && p->payload_case==SEC2_PAYLOAD__PAYLOAD_SC1 && p->sc1 && p->sc1->client_proof.len==64)command=1;
        }
        if(message)session_data__free_unpacked(message,NULL);
        if(command==99){++state.failures;secure_free(input,received);return fail(r,"401 Unauthorized","Invalid authentication request");}
        if(command==0) {
            /* An unrelated request must never replace an authenticated owner. */
            if(state.authenticated && !same_session(r,peer)){secure_free(input,received);return fail(r,"409 Conflict","A local session is already active");}
            drop_session(); state.session_id=esp_random();
            if(state.session_id==UINT32_MAX)state.session_id--;
            if(protocomm_open_session(state.pc,state.session_id)!=ESP_OK){secure_free(input,received);return fail(r,"503 Service Unavailable","Session allocation failed");}
            state.session_open=true;state.peer_address=peer;
            uint8_t token[32];esp_fill_random(token,sizeof(token));
            for(size_t i=0;i<sizeof(token);i++)snprintf(state.cookie+i*2,3,"%02x",token[i]);
            mbedtls_platform_zeroize(token,sizeof(token));
        } else if(!same_session(r,peer)){secure_free(input,received);return fail(r,"401 Unauthorized","Session expired");}
    } else if(!same_session(r,peer)||!state.authenticated){secure_free(input,received);return fail(r,"401 Unauthorized","Authenticate first");}
    if(++state.messages>MAX_SESSION_MESSAGES){drop_session();secure_free(input,received);return fail(r,"401 Unauthorized","Reauthenticate");}
    esp_err_t result=protocomm_req_handle(state.pc,handshake?"prov-session":"prov-network",state.session_id,input,received,&output,&output_len);
    secure_free(input,received);
    if(result!=ESP_OK){++state.failures;drop_session();secure_free(output,output_len>0?(size_t)output_len:0);return fail(r,"401 Unauthorized","Authentication or protected request failed");}
    if(handshake && command==1){state.authenticated=true;touch();}
    headers(r,"application/octet-stream");
    if(handshake && command==0){char cookie[130];snprintf(cookie,sizeof(cookie),"gw_session=%s; HttpOnly; SameSite=Strict; Path=/",state.cookie);httpd_resp_set_hdr(r,"Set-Cookie",cookie);}
    result=httpd_resp_send(r,(const char *)output,output_len);secure_free(output,output_len>0?(size_t)output_len:0);
    /* A lost encrypted response is ambiguous: the browser reauthenticates and
     * queries its durable request/transaction, never resends ciphertext. */
    return result;
}

static void stop_locked(void) {
    portENTER_CRITICAL(&state_lock);state.open=false;portEXIT_CRITICAL(&state_lock);
    gw_core_set_local_window_deadline(0);
    if(state.server){httpd_stop(state.server);state.server=NULL;}
    if(state.pc){drop_session();protocomm_delete(state.pc);state.pc=NULL;}
    gw_core_ap_stop();
    mbedtls_platform_zeroize(state.salt,sizeof(state.salt));
    mbedtls_platform_zeroize(state.verifier,sizeof(state.verifier));
}
static void supervisor(void *arg) {
    (void)arg;
    for(;;){
        vTaskDelay(pdMS_TO_TICKS(500));xSemaphoreTake(state.lifecycle,portMAX_DELAY);
        if(state.server && active() && !state.commit_grace){
            cJSON *snapshot=gw_core_state();
            const cJSON *revision=cJSON_GetObjectItemCaseSensitive(snapshot,"localRevision");
            const cJSON *verified=cJSON_GetObjectItemCaseSensitive(snapshot,"verificationState");
            if(cJSON_IsString(revision) && cJSON_IsString(verified) && strcmp(revision->valuestring,state.opening_revision) && !strcmp(verified->valuestring,"PLATFORM_CONNECTED")){
                uint64_t grace=gw_core_mono_ms()+60000;
                portENTER_CRITICAL(&state_lock);if(state.deadline>grace)state.deadline=grace;portEXIT_CRITICAL(&state_lock);
                state.commit_grace=true;
            }
            cJSON_Delete(snapshot);
        }
        if(state.server && !active())stop_locked();
        xSemaphoreGive(state.lifecycle);
    }
}
esp_err_t gw_provision_init(void) {
    if(state.lifecycle)return ESP_OK;
    esp_log_level_set("security2",ESP_LOG_WARN);esp_log_level_set("esp_srp",ESP_LOG_WARN);
    state.lifecycle=xSemaphoreCreateMutex();if(!state.lifecycle)return ESP_ERR_NO_MEM;
    if(xTaskCreate(supervisor,"gw_prov_window",4096,NULL,3,NULL)!=pdPASS){vSemaphoreDelete(state.lifecycle);state.lifecycle=NULL;return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
esp_err_t gw_provision_open_window(uint32_t seconds) {
    if(!state.lifecycle || !seconds || seconds>1800 || protocomm_security2.patch_ver!=1)return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(state.lifecycle,portMAX_DELAY);
    if(state.server){xSemaphoreGive(state.lifecycle);return ESP_ERR_INVALID_STATE;}
    size_t salt_size=sizeof(state.salt),verifier_size=sizeof(state.verifier);
    esp_err_t result=gw_factory_get_string("setup_user",state.username,sizeof(state.username));
    if(result==ESP_OK)result=gw_factory_get_blob("sec2_salt",state.salt,&salt_size);
    if(result==ESP_OK)result=gw_factory_get_blob("sec2_verifier",state.verifier,&verifier_size);
    if(result!=ESP_OK || !state.username[0] || salt_size!=16 || verifier_size!=384){result=ESP_ERR_INVALID_STATE;goto failure;}
    result=gw_core_ap_start();if(result!=ESP_OK)goto failure;
    state.pc=protocomm_new();if(!state.pc){result=ESP_ERR_NO_MEM;goto failure;}
    state.params=(protocomm_security2_params_t){.salt=(const char *)state.salt,.salt_len=16,.verifier=(const char *)state.verifier,.verifier_len=384};
    result=protocomm_set_security(state.pc,"prov-session",&protocomm_security2,&state.params);if(result!=ESP_OK)goto failure;
    result=protocomm_add_endpoint(state.pc,"prov-network",network_handler,NULL);if(result!=ESP_OK)goto failure;
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.max_uri_handlers=6;config.max_open_sockets=3;config.lru_purge_enable=true;
    config.stack_size=12288;config.recv_wait_timeout=5;config.send_wait_timeout=5;
    result=httpd_start(&state.server,&config);if(result!=ESP_OK)goto failure;
    const httpd_uri_t handlers[]={
        {.uri="/",.method=HTTP_GET,.handler=asset},{.uri="/app.js",.method=HTTP_GET,.handler=asset},
        {.uri="/style.css",.method=HTTP_GET,.handler=asset},{.uri="/prov-info",.method=HTTP_GET,.handler=info},
        {.uri="/prov-session",.method=HTTP_POST,.handler=post},{.uri="/prov-network",.method=HTTP_POST,.handler=post}};
    for(size_t i=0;i<sizeof(handlers)/sizeof(handlers[0]);i++){result=httpd_register_uri_handler(state.server,&handlers[i]);if(result!=ESP_OK)goto failure;}
    state.failures=0;state.rate_count=0;state.rate_started=0;state.commit_grace=false;
    cJSON *snapshot=gw_core_state();const cJSON *revision=cJSON_GetObjectItemCaseSensitive(snapshot,"localRevision");
    snprintf(state.opening_revision,sizeof(state.opening_revision),"%s",cJSON_IsString(revision)?revision->valuestring:"0");cJSON_Delete(snapshot);
    portENTER_CRITICAL(&state_lock);
    state.close_requested=false;state.open=true;state.deadline=gw_core_mono_ms()+(uint64_t)seconds*1000;
    state.last_authenticated=gw_core_mono_ms();
    portEXIT_CRITICAL(&state_lock);
    gw_core_set_local_window_deadline(state.deadline);
    xSemaphoreGive(state.lifecycle);return ESP_OK;
failure:
    stop_locked();xSemaphoreGive(state.lifecycle);return result;
}
void gw_provision_close_window(void) {portENTER_CRITICAL(&state_lock);state.close_requested=true;portEXIT_CRITICAL(&state_lock);}
bool gw_provision_is_open(void) {return active();}
