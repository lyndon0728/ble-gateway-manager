#include "gw_nrf.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

#define PORT UART_NUM_1
typedef struct { uint8_t boot[16]; struct gw_frame frame; } batch_t;
static SemaphoreHandle_t request_lock, tx_lock, identity_lock, io_lock;
static QueueHandle_t responses, batches, events;
static gw_nrf_batch_fn receive_batch;
static void *receive_context;
static uint64_t link_id;
static uint32_t next_id = 1, capabilities;
static uint8_t boot_id[16];
static char firmware[64];
static bool ready;
static uint64_t bad_frames, busy_batches;

int64_t gw_nrf_now_ms(void) { return esp_timer_get_time() / 1000; }
static TickType_t remaining(int64_t deadline) {
    int64_t ms = deadline - gw_nrf_now_ms();
    return ms <= 0 ? 0 : pdMS_TO_TICKS(ms) + 1;
}
static esp_err_t transmit(const struct gw_frame *f, int64_t deadline) {
    uint8_t *wire = malloc(GW_WIRE_MAX);
    if (!wire) return ESP_ERR_NO_MEM;
    size_t n = gw_encode_frame(f, wire, GW_WIRE_MAX);
    if (!n) { free(wire); return ESP_ERR_INVALID_ARG; }
    if (!remaining(deadline) || xSemaphoreTake(tx_lock, remaining(deadline)) != pdTRUE) {
        free(wire); return ESP_ERR_TIMEOUT;
    }
    /* IDF TX ring owns a copy after uart_write_bytes; mutex extends through
     * completion. No next frame starts while old queued bytes remain. */
    esp_err_t e = ESP_FAIL;
    if (remaining(deadline) && uart_write_bytes(PORT, wire, n) == (int)n)
        e = uart_wait_tx_done(PORT, remaining(deadline));
    if (e != ESP_OK) {
        /* No TX buffer cancellation API: stop/restart is not safe here.
         * Hold serialization until all queued bytes drain; CTS watchdog is
         * handled by a bounded driver reset under this exclusive lock. */
        xSemaphoreTake(io_lock, portMAX_DELAY);
        uart_driver_delete(PORT);
        esp_err_t installed = uart_driver_install(PORT, 8192, 4096, 24, &events, 0);
        if (installed != ESP_OK) events = NULL;
        uart_set_hw_flow_ctrl(PORT, UART_HW_FLOWCTRL_CTS_RTS, 96);
        xSemaphoreGive(io_lock);
        xSemaphoreTake(identity_lock, portMAX_DELAY); ready = false;
        xSemaphoreGive(identity_lock);
    }
    xSemaphoreGive(tx_lock);
    free(wire);
    return e;
}
bool gw_nrf_identity(uint8_t out[16], uint32_t *caps, char version[64]) {
    if (!identity_lock) return false;
    xSemaphoreTake(identity_lock, portMAX_DELAY);
    bool valid = ready;
    if (out) memcpy(out, boot_id, 16);
    if (caps) *caps = capabilities;
    if (version) memcpy(version, firmware, 64);
    xSemaphoreGive(identity_lock);
    return valid;
}
static void rx_task(void *unused) {
    (void)unused;
    struct gw_decoder *decoder = calloc(1, sizeof(*decoder));
    struct gw_frame *f = malloc(sizeof(*f));
    uint8_t bytes[256];
    if (!decoder || !f) { free(decoder); free(f); vTaskDelete(NULL); return; }
    for (;;) {
        xSemaphoreTake(io_lock, portMAX_DELAY);
        int n = events ? uart_read_bytes(PORT, bytes, sizeof(bytes), pdMS_TO_TICKS(20)) : 0;
        uart_event_t event;
        while (events && xQueueReceive(events, &event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR) {
                ++bad_frames; decoder->length = 0; decoder->discard = true;
            }
        }
        xSemaphoreGive(io_lock);
        if (!n) { vTaskDelay(1); continue; }
        for (int i = 0; i < n; ++i) {
            int rc = gw_decoder_feed(decoder, bytes[i], f);
            if (rc < 0) { ++bad_frames; continue; }
            if (rc != 1) continue;
            xSemaphoreTake(identity_lock, portMAX_DELAY);
            bool match = f->link_id == link_id;
            bool online = ready;
            xSemaphoreGive(identity_lock);
            if (!match) continue;
            if (f->type == 0x81 || f->type == 0x90) {
                /* A response belongs to the sole in-flight request. */
                (void)xQueueOverwrite(responses, f);
            } else if (online && f->type >= 0x20 && f->type <= 0x22) {
                batch_t *b = malloc(sizeof(*b));
                if (b) {
                    gw_nrf_identity(b->boot, NULL, NULL);
                    b->frame = *f;
                    if (xQueueSend(batches, &b, 0) == pdTRUE) continue;
                    free(b);
                }
                ++busy_batches;
                /* Peer keeps its frozen batch and retries; withholding ACK
                 * under RAM pressure is safe and avoids blocking RX. */
            }
        }
    }
}
static void batch_task(void *unused) {
    (void)unused;
    for (;;) {
        batch_t *b;
        if (xQueueReceive(batches, &b, portMAX_DELAY) != pdTRUE) continue;
        uint8_t status = receive_batch ? receive_batch(b->boot, &b->frame, receive_context) : 1;
        if (status == 255) { free(b); continue; }
        if (status > 2) status = 2;
        struct gw_frame *ack = calloc(1, sizeof(*ack));
        if (ack) {
            ack->type = 0xa0; ack->link_id = b->frame.link_id;
            ack->msg_id = b->frame.msg_id; ack->payload_len = 3;
            ack->payload[0] = status; gw_put_u16(ack->payload + 1, status == 1 ? 500 : 0);
            transmit(ack, gw_nrf_now_ms() + 500);
            free(ack);
        }
        free(b);
    }
}
esp_err_t gw_nrf_rehandshake(int64_t deadline) {
    if (!request_lock || !remaining(deadline) ||
        xSemaphoreTake(request_lock, remaining(deadline)) != pdTRUE) return ESP_ERR_TIMEOUT;
    struct gw_frame *f = calloc(1, sizeof(*f)), *r = malloc(sizeof(*r));
    esp_err_t e = ESP_ERR_NO_MEM;
    if (!f || !r) goto done;
    xSemaphoreTake(identity_lock, portMAX_DELAY);
    esp_fill_random(&link_id, sizeof(link_id)); if (!link_id) link_id = 1;
    f->link_id = link_id; ready = false;
    xSemaphoreGive(identity_lock);
    f->type = 1; next_id = 1; xQueueReset(responses);
    for (uint32_t backoff = 250; remaining(deadline); backoff = backoff < 2000 ? backoff * 2 : 2000) {
        e = transmit(f, deadline);
        if (e != ESP_OK) break;
        int64_t until = gw_nrf_now_ms() + backoff;
        if (until > deadline) until = deadline;
        while (remaining(until) && xQueueReceive(responses, r, remaining(until)) == pdTRUE) {
            if (r->type != 0x81 || r->msg_id || r->link_id != f->link_id) continue;
            if (r->payload_len < 22 || r->payload[20] < 1 || r->payload[20] > 63 ||
                r->payload_len != (size_t)(21 + r->payload[20])) continue;
            xSemaphoreTake(identity_lock, portMAX_DELAY);
            memcpy(boot_id, r->payload, 16); capabilities = gw_get_u32(r->payload + 16);
            memset(firmware, 0, sizeof(firmware)); memcpy(firmware, r->payload + 21, r->payload[20]);
            ready = true;
            xSemaphoreGive(identity_lock); e = ESP_OK; goto done;
        }
        e = ESP_ERR_TIMEOUT;
    }
done:
    free(f); free(r); xSemaphoreGive(request_lock); return e;
}
esp_err_t gw_nrf_command(uint8_t op, const void *args, size_t size,
                        uint8_t *status, void *result, size_t *out_size, int64_t deadline) {
    if (size > GW_PAYLOAD_MAX - 1 || (size && !args) || !out_size || !status) return ESP_ERR_INVALID_ARG;
    if (!request_lock || !remaining(deadline) || xSemaphoreTake(request_lock, remaining(deadline)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t e = ESP_ERR_INVALID_STATE;
    struct gw_frame *f = NULL, *r = NULL;
    if (!gw_nrf_identity(NULL, NULL, NULL) || next_id == UINT32_MAX) goto done;
    f = calloc(1, sizeof(*f)); r = malloc(sizeof(*r));
    if (!f || !r) { e = ESP_ERR_NO_MEM; goto done; }
    f->type = 0x10; f->link_id = link_id; f->msg_id = next_id++;
    f->payload_len = 1 + size; f->payload[0] = op; if (size) memcpy(f->payload + 1, args, size);
    xQueueReset(responses);
    /* TIME_PROBE is deliberately never retransmitted: its reply cannot then
     * be mistaken for the original timing sample. */
    for (unsigned attempt = 0; attempt < (op == 4 ? 1U : 3U) && remaining(deadline); ++attempt) {
        e = transmit(f, deadline); if (e != ESP_OK) break;
        int64_t until = gw_nrf_now_ms() + (op >= 0x30 && op <= 0x37 ? 30000 : 500);
        if (until > deadline) until = deadline;
        while (remaining(until) && xQueueReceive(responses, r, remaining(until)) == pdTRUE) {
            if (r->type != 0x90 || r->link_id != f->link_id || r->msg_id != f->msg_id ||
                r->payload_len < 2 || r->payload[0] != op) continue;
            *status = r->payload[1];
            if (*status > 7 || (*status && r->payload_len != 2)) continue;
            size_t n = r->payload_len - 2;
            if (n > *out_size || (n && !result)) { e = ESP_ERR_INVALID_SIZE; goto done; }
            if (n) memcpy(result, r->payload + 2, n);
            *out_size = n; e = ESP_OK; goto done;
        }
        e = ESP_ERR_TIMEOUT;
    }
    if (e != ESP_OK) {
        xSemaphoreTake(identity_lock, portMAX_DELAY); ready = false; xSemaphoreGive(identity_lock);
    }
done:
    free(f); free(r); xSemaphoreGive(request_lock); return e;
}
static void monitor(void *unused) {
    (void)unused;
    for (;;) {
        if (!gw_nrf_identity(NULL, NULL, NULL)) gw_nrf_rehandshake(gw_nrf_now_ms() + 2500);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
esp_err_t gw_nrf_init(gw_nrf_batch_fn callback, void *ctx) {
    request_lock = xSemaphoreCreateMutex(); tx_lock = xSemaphoreCreateMutex(); identity_lock = xSemaphoreCreateMutex(); io_lock = xSemaphoreCreateMutex();
    responses = xQueueCreate(1, sizeof(struct gw_frame)); batches = xQueueCreate(4, sizeof(batch_t *));
    if (!request_lock || !tx_lock || !identity_lock || !io_lock || !responses || !batches) return ESP_ERR_NO_MEM;
    receive_batch = callback; receive_context = ctx;
    uart_config_t cfg = {.baud_rate = 1000000, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS, .rx_flow_ctrl_thresh = 96, .source_clk = UART_SCLK_DEFAULT};
    esp_err_t e = uart_param_config(PORT, &cfg); if (e) return e;
    e = uart_set_pin(PORT, 17, 18, 15, 16); if (e) return e;
    /* Release reset. No reset is performed just because a serial handshake failed. */
    gpio_config_t reset = {.pin_bit_mask = 1ULL << 8, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE};
    if ((e = gpio_config(&reset)) != ESP_OK) return e;
    if ((e = uart_driver_install(PORT, 8192, 4096, 24, &events, 0)) != ESP_OK) return e;
    if (xTaskCreate(rx_task, "nrf_rx", 10240, NULL, 12, NULL) != pdPASS ||
        xTaskCreate(batch_task, "nrf_custody", 8192, NULL, 8, NULL) != pdPASS ||
        xTaskCreate(monitor, "nrf_link", 8192, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/* Configuration conversion is kept separate from the transport. */
static double number(const cJSON *o, const char *key, double fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}
static bool uuid(const char *s, uint8_t out[16]) {
    char compact[33]; size_t n = 0;
    if (!s) return false;
    if (strlen(s) == 4) { char expanded[37]; snprintf(expanded, sizeof(expanded), "0000%s-0000-1000-8000-00805f9b34fb", s); return uuid(expanded, out); }
    if (strlen(s) == 8) { char expanded[37]; snprintf(expanded, sizeof(expanded), "%s-0000-1000-8000-00805f9b34fb", s); return uuid(expanded, out); }
    if (strlen(s) != 36) return false;
    for (size_t i = 0; s[i]; ++i) {
        if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;continue;}
        if(!isxdigit((unsigned char)s[i])||n>=32)return false;compact[n++]=s[i];
    }
    if (n != 32) return false; compact[32] = 0;
    for (size_t i = 0; i < 16; ++i) { char a[3] = {compact[2*i], compact[2*i+1], 0}; char *end;
        unsigned long v = strtoul(a, &end, 16); if (*end) return false; out[i] = v; }
    return true;
}
static int uuid_cmp(const void *a, const void *b) { return memcmp(a,b,16); }
static int company_cmp(const void *a, const void *b) { uint16_t x=*(const uint16_t*)a,y=*(const uint16_t*)b; return (x>y)-(x<y); }
static bool b64(const cJSON *o, const char *key, uint8_t *out, size_t cap, size_t *n) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if (!cJSON_IsString(v)) return false;
    return mbedtls_base64_decode(out,cap,n,(const unsigned char*)v->valuestring,strlen(v->valuestring))==0;
}
static esp_err_t set_readback(uint8_t set, uint8_t get, const uint8_t *data, size_t n, int64_t deadline) {
    uint8_t *result=malloc(GW_PAYLOAD_MAX); if (!result) return ESP_ERR_NO_MEM;
    uint8_t status=0xff; size_t sz=GW_PAYLOAD_MAX;
    esp_err_t e=gw_nrf_command(set,data,n,&status,result,&sz,deadline);
    if (e==ESP_OK && (status || sz!=n || memcmp(result,data,n))) e=ESP_FAIL;
    if (e==ESP_OK) { sz=GW_PAYLOAD_MAX; e=gw_nrf_command(get,NULL,0,&status,result,&sz,deadline);
        if (e==ESP_OK && (status || sz!=n || memcmp(result,data,n))) e=ESP_FAIL; }
    free(result); return e;
}
esp_err_t gw_nrf_scan(const cJSON *complete, uint64_t revision, int64_t deadline) {
    struct gw_profile *p=calloc(1,sizeof(*p)); uint8_t *data=malloc(GW_PROFILE_MAX);
    if (!p || !data) {free(p);free(data);return ESP_ERR_NO_MEM;}
    gw_profile_default(p); p->revision=revision;
    const cJSON *scan=cJSON_GetObjectItemCaseSensitive(complete,"scan"), *filters=cJSON_GetObjectItemCaseSensitive(complete,"filters");
    esp_err_t e=ESP_ERR_INVALID_ARG;
    if (!cJSON_IsObject(scan) || !cJSON_IsObject(filters) || !revision) goto done;
    double interval=number(scan,"intervalMs",100),window=number(scan,"windowMs",50),rssi=number(filters,"minRssi",-128);
    if (interval<10||interval>10240||window<10||window>interval||floor(interval)!=interval||floor(window)!=window||rssi < -128||rssi>20||floor(rssi)!=rssi) goto done;
    p->interval_ticks=((uint32_t)interval*8+2)/5; p->window_ticks=((uint32_t)window*8+2)/5; p->min_rssi=(int8_t)rssi;
    p->scan_flags=(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(scan,"enabled"))?1:0)|(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(scan,"active"))?2:0);
    const cJSON *v,*list=cJSON_GetObjectItemCaseSensitive(filters,"serviceUuids");
    cJSON_ArrayForEach(v,list) { if (p->uuid_count==128||!cJSON_IsString(v)||!uuid(v->valuestring,p->uuids[p->uuid_count])) goto done; ++p->uuid_count; }
    qsort(p->uuids,p->uuid_count,16,uuid_cmp); unsigned used=0;
    for(unsigned i=0;i<p->uuid_count;i++) if(!used||memcmp(p->uuids[i],p->uuids[used-1],16)) memcpy(p->uuids[used++],p->uuids[i],16);
    p->uuid_count=used; list=cJSON_GetObjectItemCaseSensitive(filters,"manufacturerIds");
    cJSON_ArrayForEach(v,list) { if(p->company_count==128||!cJSON_IsNumber(v)||v->valuedouble<0||v->valuedouble>65535||floor(v->valuedouble)!=v->valuedouble)goto done; p->companies[p->company_count++]=(uint16_t)v->valuedouble; }
    qsort(p->companies,p->company_count,2,company_cmp); used=0;
    for(unsigned i=0;i<p->company_count;i++)if(!used||p->companies[i]!=p->companies[used-1])p->companies[used++]=p->companies[i];
    p->company_count=used;
    list=cJSON_GetObjectItemCaseSensitive(complete,"beaconRules");
    cJSON_ArrayForEach(v,list) {
        if(p->rule_count==8)goto done;
        struct gw_beacon_rule *rule=&p->rules[p->rule_count];
        const cJSON *action=cJSON_GetObjectItemCaseSensitive(v,"action");
        if(!cJSON_IsString(action)||(strcmp(action->valuestring,"ALLOW")&&strcmp(action->valuestring,"DENY")))goto done;
        rule->action=!strcmp(action->valuestring,"ALLOW");rule->kind_mask=0;
        const cJSON *kind,*kinds=cJSON_GetObjectItemCaseSensitive(v,"reportKinds");
        cJSON_ArrayForEach(kind,kinds) {
            if(!cJSON_IsString(kind))goto done;
            const char *names[]={"LEGACY_ADV","LEGACY_SCAN_RSP","EXT_ADV","EXT_SCAN_RSP"};bool known=false;
            for(unsigned k=0;k<4;k++)if(!strcmp(kind->valuestring,names[k])){rule->kind_mask|=1U<<k;known=true;}
            if(!known)goto done;
        }
        double type=number(v,"adType",0),offset=number(v,"offset",-1);
        if(type<1||type>255||floor(type)!=type||offset<0||offset>253||floor(offset)!=offset)goto done;
        rule->ad_type=(uint8_t)type;rule->offset=(uint16_t)offset;
        size_t n=0,m=0;if(!b64(v,"valueBase64",rule->value,24,&n)||!b64(v,"maskBase64",rule->mask,24,&m)||n!=m||!n)goto done;
        if(!rule->kind_mask||offset+n>254)goto done;bool any=false;for(size_t k=0;k<n;k++)any|=rule->mask[k]!=0;if(!any)goto done;
        rule->length=n;p->rule_count++;
    }
    size_t n=gw_profile_encode(p,true,data,GW_PROFILE_MAX); if(!n)goto done;
    uint32_t caps=0;gw_nrf_identity(NULL,&caps,NULL);
    if(!(caps&(1U<<1))){e=ESP_ERR_NOT_SUPPORTED;goto done;}
    e=set_readback(5,6,data,n,deadline);
done: free(p);free(data);return e;
}
esp_err_t gw_nrf_advertise(const cJSON *adv, uint64_t revision, int64_t deadline) {
    uint8_t data[76]; size_t n=0,m=0; uint32_t caps=0;gw_nrf_identity(NULL,&caps,NULL);
    if(!(caps&(1U<<4)))return ESP_ERR_NOT_SUPPORTED;
    const cJSON *mode=cJSON_GetObjectItemCaseSensitive(adv,"mode");
    if(!cJSON_IsString(mode)||!revision)return ESP_ERR_INVALID_ARG;
    uint8_t kind=!strcmp(mode->valuestring,"NONCONNECTABLE")?0:!strcmp(mode->valuestring,"SCANNABLE_NONCONNECTABLE")?1:!strcmp(mode->valuestring,"CONNECTABLE_SCANNABLE")?2:255;
    if(kind==255||(kind==2&&!(caps&(1U<<11))))return ESP_ERR_NOT_SUPPORTED;
    double interval=number(adv,"intervalMs",100);
    if(interval<100||interval>10240||floor(interval)!=interval)return ESP_ERR_INVALID_ARG;
    gw_put_u64(data,revision);data[8]=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(adv,"enabled"));data[9]=kind;gw_put_u16(data+10,((uint32_t)interval*8+2)/5);
    if(!b64(adv,"advDataBase64",data+13,31,&n)||!gw_ad_valid(data+13,n))return ESP_ERR_INVALID_ARG;
    data[12]=n;
    if(!b64(adv,"scanResponseBase64",data+14+n,31,&m)||!gw_ad_valid(data+14+n,m)||(kind==0&&m))return ESP_ERR_INVALID_ARG;
    data[13+n]=m;return set_readback(7,8,data,14+n+m,deadline);
}
cJSON *gw_nrf_diagnostics(int64_t deadline) {
    cJSON *o=cJSON_CreateObject(); uint8_t boot[16],result[71],status;uint32_t caps=0;char version[64]={0};size_t n=sizeof(result);
    if(!o)return NULL;
    cJSON_AddBoolToObject(o,"online",gw_nrf_identity(boot,&caps,version));
    cJSON_AddNumberToObject(o,"badFrames",bad_frames);cJSON_AddNumberToObject(o,"busyBatches",busy_batches);
    cJSON_AddNumberToObject(o,"capabilities",caps);cJSON_AddStringToObject(o,"firmwareVersion",version);
    if(gw_nrf_command(3,NULL,0,&status,result,&n,deadline)==ESP_OK&&!status&&n==71) {
        cJSON_AddNumberToObject(o,"queuedRecords",gw_get_u16(result+17));cJSON_AddNumberToObject(o,"queuedBytes",gw_get_u32(result+19));
        cJSON_AddNumberToObject(o,"scanState",result[16]);
    }
    return o;
}
