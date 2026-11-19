#include "gw_capture.h"
#include "esp_flash_encrypt.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_capture_rules.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_transport.h"
#include "gw_nrf.h"
#include "gw_spool.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

typedef struct {
  bool present, mapped, comparable;
  uint8_t type, radio, key[32];
  uint16_t length;
  uint32_t error;
  uint64_t arrived, mapped_ms, order;
} pending_t;
typedef struct {
  char event[80], address[13];
  uint8_t boot[16], address_type;
  uint64_t utc_ms;
} evidence_t;
static const esp_partition_t *partition;
static SemaphoreHandle_t lock, upload_lock;
static gw_spool_t spool;
static pending_t pending[GW_SPOOL_SLOTS];
static evidence_t evidence[64];
static unsigned evidence_next;
static uint8_t esp_boot[16];
static bool radios[2] = {true, false}, initialized;
static struct {
  bool valid;
  uint8_t boot[16];
  uint64_t nrf_ms, esp_ms;
  uint32_t error;
} anchor;
static uint64_t utc_anchor_mono, utc_anchor_wall, next_upload;
static bool utc_anchor_valid;
static unsigned upload_failures;
static unsigned upload_batch_max = 64, upload_flush_ms = 250;
static uint64_t accepted_nrf, accepted_esp, merged_count, unmapped_count,
    recovered_count;
static const char *reasons[] = {"DISCONNECTED", "NRF_RESTART", "ESP_RESTART"};
bool gw_capture_upload_valid(const cJSON *complete) {
  if (!cJSON_IsObject(complete))
    return false;
  unsigned seen = 0;
  for (const cJSON *v = complete->child; v; v = v->next) {
    unsigned field = v->string && !strcmp(v->string, "batchMaxRecords")   ? 1
                     : v->string && !strcmp(v->string, "flushIntervalMs") ? 2
                                                                          : 0;
    if (!field || (seen & field) || !cJSON_IsNumber(v) ||
        !isfinite(v->valuedouble) || floor(v->valuedouble) != v->valuedouble ||
        v->valuedouble < (field == 1 ? 1 : 100) ||
        v->valuedouble > (field == 1 ? 1000 : 60000))
      return false;
    seen |= field;
  }
  return seen == 3;
}
static void install_upload(const cJSON *complete) {
  upload_batch_max =
      (unsigned)cJSON_GetObjectItemCaseSensitive(complete, "batchMaxRecords")
          ->valuedouble;
  upload_flush_ms =
      (unsigned)cJSON_GetObjectItemCaseSensitive(complete, "flushIntervalMs")
          ->valuedouble;
}
static esp_err_t restore_upload(void) {
  cJSON *saved = gw_store_get_json("capture/upload");
  if (!saved) {
    size_t size = 0;
    if (gw_store_get("capture/upload", NULL, &size) != ESP_ERR_NOT_FOUND)
      return ESP_ERR_INVALID_STATE;
    cJSON *configuration = gw_store_get_json("cloud/configuration");
    const cJSON *upload =
        cJSON_GetObjectItemCaseSensitive(configuration, "upload");
    saved = upload ? cJSON_Duplicate(upload, true) : cJSON_CreateObject();
    cJSON_Delete(configuration);
    if (!saved)
      return ESP_ERR_NO_MEM;
    if (!cJSON_GetObjectItemCaseSensitive(saved, "batchMaxRecords"))
      cJSON_AddNumberToObject(saved, "batchMaxRecords", 64);
    if (!cJSON_GetObjectItemCaseSensitive(saved, "flushIntervalMs"))
      cJSON_AddNumberToObject(saved, "flushIntervalMs", 250);
  }
  if (!gw_capture_upload_valid(saved)) {
    cJSON_Delete(saved);
    return ESP_ERR_INVALID_STATE;
  }
  install_upload(saved);
  cJSON_Delete(saved);
  return ESP_OK;
}
esp_err_t gw_capture_configure_upload(const cJSON *complete) {
  if (!initialized)
    return ESP_ERR_INVALID_STATE;
  if (!gw_capture_upload_valid(complete))
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(lock, portMAX_DELAY);
  esp_err_t e = gw_store_put_json("capture/upload", complete);
  if (e == ESP_OK)
    install_upload(complete);
  xSemaphoreGive(lock);
  return e;
}
cJSON *gw_capture_upload_configuration(void) {
  if (!initialized)
    return NULL;
  xSemaphoreTake(lock, portMAX_DELAY);
  cJSON *o = cJSON_CreateObject();
  if (o) {
    cJSON_AddNumberToObject(o, "batchMaxRecords", upload_batch_max);
    cJSON_AddNumberToObject(o, "flushIntervalMs", upload_flush_ms);
  }
  xSemaphoreGive(lock);
  return o;
}
static const char *str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static uint64_t mono(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static void hex(const uint8_t *p, size_t n, char *out, bool upper) {
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[p[i] >> 4];
    out[2 * i + 1] = digits[p[i] & 15];
  }
  out[2 * n] = 0;
}
static uint64_t wall_ms(void) {
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec > 0
             ? (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_usec / 1000
             : 0;
}
static esp_err_t result(int status) {
  return status == GW_SP_OK || status == GW_SP_REPLAY ? ESP_OK
         : status == GW_SP_FULL                       ? ESP_ERR_NO_MEM
                                                      : ESP_FAIL;
}
static int io_read(void *ctx, uint32_t offset, void *data, size_t n) {
  return esp_partition_read(ctx, offset, data, n) != ESP_OK;
}
static int io_write(void *ctx, uint32_t offset, const void *data, size_t n) {
  return esp_partition_write(ctx, offset, data, n) != ESP_OK;
}
static int io_erase(void *ctx, uint32_t offset, size_t n) {
  return esp_partition_erase_range(ctx, offset, n) != ESP_OK;
}
static int io_erased(void *ctx, uint32_t offset, size_t n, bool *empty) {
  uint8_t buffer[256];
  *empty = false;
  while (n) {
    size_t take = n < sizeof(buffer) ? n : sizeof(buffer);
    if (esp_partition_read_raw(ctx, offset, buffer, take) != ESP_OK)
      return 1;
    for (size_t i = 0; i < take; i++)
      if (buffer[i] != 255)
        return 0;
    offset += take;
    n -= take;
  }
  *empty = true;
  return 0;
}
static void clock_sample(uint64_t now) {
  if (!gw_core_time_trusted()) {
    utc_anchor_valid = false;
    return;
  }
  uint64_t wall = wall_ms();
  if (!wall) {
    utc_anchor_valid = false;
    return;
  }
  uint64_t expected =
      utc_anchor_wall + (now >= utc_anchor_mono ? now - utc_anchor_mono : 0);
  uint64_t difference = wall > expected ? wall - expected : expected - wall;
  if (!utc_anchor_valid || now < utc_anchor_mono || difference > 1000) {
    utc_anchor_valid = true;
    utc_anchor_mono = now;
    utc_anchor_wall = wall;
  }
}
static void stamp(gw_spool_record_t *v, uint64_t now) {
  v->arrived_ms = now;
  clock_sample(now);
  uint64_t captured = gw_get_u64(v->bytes);
  if (v->radio == 1) {
    v->mapped = captured <= now && now - captured <= 60000;
    v->mapped_ms = captured;
    v->error_ms = 1;
  } else if (anchor.valid && !memcmp(anchor.boot, v->source_boot, 16) &&
             now >= anchor.esp_ms && now - anchor.esp_ms <= 60000)
    v->mapped = gw_capture_map_time(captured, anchor.nrf_ms, anchor.esp_ms,
                                    anchor.error, &v->mapped_ms, &v->error_ms);
  if (v->mapped && utc_anchor_valid && v->mapped_ms >= utc_anchor_mono &&
      v->mapped_ms - utc_anchor_mono <= 60000) {
    v->utc_ms = utc_anchor_wall + (v->mapped_ms - utc_anchor_mono);
  } else if (v->mapped && utc_anchor_valid && now >= utc_anchor_mono &&
             now - utc_anchor_mono > 60000) {
    /* Stable UTC offset verified above, refresh without discarding the last
     * minute of captures. A detected clock jump always starts a new epoch. */
    utc_anchor_wall += now - utc_anchor_mono;
    utc_anchor_mono = now;
    if (v->mapped_ms <= now && now - v->mapped_ms <= 60000 &&
        utc_anchor_wall > now - v->mapped_ms)
      v->utc_ms = utc_anchor_wall - (now - v->mapped_ms);
  }
  if (!v->mapped)
    unmapped_count++;
}
void gw_capture_esp_boot(uint8_t boot[16]) {
  if (boot)
    memcpy(boot, esp_boot, 16);
}
void gw_capture_connection_ref(uint8_t radio, const uint8_t boot[16],
                               uint32_t id, char out[33]) {
  uint8_t input[33], digest[32];
  memcpy(input, "gw32-conn-v1", 12);
  input[12] = radio;
  memcpy(input + 13, boot, 16);
  gw_put_u32(input + 29, id);
  mbedtls_sha256(input, sizeof(input), digest, 0);
  hex(digest, 16, out, false);
}
static void event_id(unsigned slot, const gw_spool_record_t *r, char out[80]) {
  if (r->type == 0x23) {
    char ref[33];
    gw_capture_connection_ref(r->radio, r->source_boot,
                              gw_get_u32(r->bytes + 8), ref);
    snprintf(out, 80, "link-%s-%s", ref, reasons[r->bytes[21]]);
    return;
  }
  char boot[33];
  hex(gw_spool_event_boot(&spool, slot), 16, boot, false);
  snprintf(out, 80, "%s-%" PRIu64, boot, gw_spool_sequence(&spool, slot));
}
static void remember(unsigned slot, const gw_spool_record_t *r) {
  const uint8_t *p = r->bytes;
  if (r->radio || r->type != 0x20 || (p[8] & 1) || !(p[9] & 1) ||
      p[10] == 255 || !r->utc_ms)
    return;
  char id[80];
  event_id(slot, r, id);
  for (unsigned i = 0; i < 64; i++)
    if (!strcmp(evidence[i].event, id))
      return;
  evidence_t *e = &evidence[evidence_next++ % 64];
  memset(e, 0, sizeof(*e));
  strcpy(e->event, id);
  memcpy(e->boot, r->source_boot, 16);
  hex(p + 11, 6, e->address, true);
  e->address_type = p[10];
  e->utc_ms = r->utc_ms;
}
static int refresh(void) {
  gw_spool_record_t *r = malloc(sizeof(*r));
  if (!r)
    return GW_SP_FULL;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++) {
    unsigned state = gw_spool_state(&spool, i);
    if (!state) {
      memset(&pending[i], 0, sizeof(pending[i]));
      continue;
    }
    if (pending[i].present)
      continue;
    int status = gw_spool_read(&spool, i, r);
    if (status) {
      free(r);
      return status;
    }
    pending_t *m = &pending[i];
    m->present = true;
    m->type = r->type;
    m->radio = r->radio;
    m->mapped = r->mapped;
    m->length = r->length;
    m->error = r->error_ms;
    m->arrived = r->arrived_ms;
    m->mapped_ms = r->mapped_ms;
    m->order = r->order;
    const uint8_t *p = r->bytes;
    m->comparable = r->type == 0x20 && p[10] != 255 &&
                    (p[8] < 2 || p[21] != 255) && r->mapped;
    if (m->comparable) {
      uint8_t key[GW_SPOOL_RECORD_MAX];
      key[0] = p[8];
      key[1] = (p[8] & 1) ? 0 : p[9] & 1;
      memcpy(key + 2, p + 10, 7);
      key[9] = p[8] < 2 ? 255 : p[21];
      memcpy(key + 10, p + 24, r->length - 24);
      mbedtls_sha256(key, r->length - 14, m->key, 0);
    }
    if (state == GW_SP_READY)
      remember(i, r);
  }
  free(r);
  return GW_SP_OK;
}
static int finalize_mode(bool unpaired, bool recovering) {
  int rc = refresh();
  if (rc)
    return rc;
  uint64_t now = mono();
  bool selected[GW_SPOOL_SLOTS] = {0};
  gw_spool_pair_t pairs[GW_SPOOL_SLOTS];
  size_t n = 0, bytes = 0;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) == GW_SP_PENDING)
      bytes += pending[i].length;
  gw_spool_record_t *a = malloc(sizeof(*a)), *b = malloc(sizeof(*b));
  if (!a || !b) {
    free(a);
    free(b);
    return GW_SP_FULL;
  }
  for (;;) {
    unsigned i = GW_SPOOL_NONE;
    uint64_t order = UINT64_MAX;
    for (unsigned j = 0; j < GW_SPOOL_SLOTS; j++)
      if (!selected[j] && gw_spool_state(&spool, j) == GW_SP_PENDING &&
          pending[j].order < order) {
        i = j;
        order = pending[j].order;
      }
    if (i == GW_SPOOL_NONE)
      break;
    pending_t *m = &pending[i];
    bool due = unpaired || !(radios[0] && radios[1]) || m->type != 0x20 ||
               !m->comparable || bytes > 128u * 1024u || now < m->arrived ||
               now - m->arrived >= 150;
    if (!due)
      break;
    if ((rc = gw_spool_read(&spool, i, a)))
      goto done;
    unsigned best = GW_SPOOL_NONE;
    uint64_t distance = UINT64_MAX, best_order = UINT64_MAX;
    if (!unpaired && radios[0] && radios[1] && m->comparable &&
        now >= m->arrived && now - m->arrived >= 150) {
      for (unsigned j = 0; j < GW_SPOOL_SLOTS; j++) {
        pending_t *v = &pending[j];
        if (i == j || selected[j] ||
            gw_spool_state(&spool, j) != GW_SP_PENDING || !v->comparable ||
            v->radio == m->radio || memcmp(m->key, v->key, 32))
          continue;
        /* A candidate arriving after this observation's deadline is late. */
        if (v->arrived > m->arrived + 150)
          continue;
        if ((rc = gw_spool_read(&spool, j, b)))
          goto done;
        uint64_t delta;
        if (gw_capture_pairable(a, b, &delta) &&
            (delta < distance ||
             (delta == distance && v->order < best_order))) {
          best = j;
          distance = delta;
          best_order = v->order;
        }
      }
    }
    selected[i] = true;
    bytes -= m->length;
    pairs[n].primary = (uint16_t)i;
    pairs[n].peer = (uint16_t)best;
    if (best != GW_SPOOL_NONE) {
      selected[best] = true;
      bytes -= pending[best].length;
      if (m->radio) {
        pairs[n].primary = (uint16_t)best;
        pairs[n].peer = (uint16_t)i;
      }
    }
    n++;
  }
  if (n) {
    rc = gw_spool_finish(&spool, pairs, n);
    if (!rc) {
      for (size_t j = 0; j < n; j++) {
        if (pairs[j].peer != GW_SPOOL_NONE) {
          memset(&pending[pairs[j].peer], 0, sizeof(pending[0]));
          merged_count++;
        }
        if (gw_spool_read(&spool, pairs[j].primary, a) == GW_SP_OK)
          remember(pairs[j].primary, a);
      }
      if (recovering)
        recovered_count += n;
    }
  }
done:
  free(a);
  free(b);
  return rc;
}
static int finalize(bool recovering) {
  return finalize_mode(recovering, recovering);
}
static int pending_room(size_t added) {
  int rc = refresh();
  if (rc)
    return rc;
  size_t bytes = added;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) == GW_SP_PENDING)
      bytes += pending[i].length;
  return bytes > 128u * 1024u ? finalize_mode(true, false) : GW_SP_OK;
}
esp_err_t gw_capture_time_anchor(const uint8_t boot[16], uint64_t tx,
                                 uint64_t rx, uint64_t nrx, uint64_t ntx,
                                 bool retried) {
  if (!initialized || !boot || retried || rx < tx || ntx < nrx ||
      rx - tx < ntx - nrx || rx - tx - (ntx - nrx) > 50)
    return ESP_ERR_INVALID_ARG;
  uint64_t net = rx - tx - (ntx - nrx);
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(anchor.boot, boot, 16);
  anchor.nrf_ms = nrx + (ntx - nrx) / 2;
  anchor.esp_ms = tx + (rx - tx) / 2;
  anchor.error = (uint32_t)((net + 1) / 2);
  anchor.valid = true;
  xSemaphoreGive(lock);
  return ESP_OK;
}
void gw_capture_radio_enabled(uint8_t radio, bool enabled) {
  if (!lock || radio > 1)
    return;
  xSemaphoreTake(lock, portMAX_DELAY);
  radios[radio] = enabled;
  xSemaphoreGive(lock);
}
static void probe_task(void *unused) {
  (void)unused;
  uint64_t next_probe = 0;
  uint8_t previous_boot[16] = {0};
  for (;;) {
    uint64_t now = mono();
    uint8_t boot[16];
    bool ready = gw_nrf_identity(boot, NULL, NULL);
    if (ready && (now >= next_probe || memcmp(boot, previous_boot, 16))) {
      /* gw_nrf_command sends TIME_PROBE exactly once. Queue time is
       * conservatively included in RTT; samples above 50ms are rejected. */
      uint8_t data[16], status = 255;
      size_t length = sizeof(data);
      uint64_t tx = mono();
      esp_err_t e = gw_nrf_command(0x04, NULL, 0, &status, data, &length,
                                   (int64_t)tx + 1000);
      uint64_t rx = mono();
      uint8_t after[16];
      bool same =
          gw_nrf_identity(after, NULL, NULL) && !memcmp(boot, after, 16);
      if (e == ESP_OK && !status && length == 16 && same)
        gw_capture_time_anchor(boot, tx, rx, gw_get_u64(data),
                               gw_get_u64(data + 8), false);
      memcpy(previous_boot, boot, 16);
      next_probe = now + 30000;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!ready || (anchor.valid && memcmp(boot, anchor.boot, 16)))
      anchor.valid = false;
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
static void timer_task(void *unused) {
  (void)unused;
  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    clock_sample(mono());
    if (!spool.fault)
      finalize(false);
    xSemaphoreGive(lock);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
esp_err_t gw_capture_init(void) {
  if (initialized)
    return ESP_OK;
  partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "gwspool");
  if (!partition || partition->size != GW_SPOOL_BYTES)
    return ESP_ERR_NOT_FOUND;
  /* Production data custody is unavailable without at-rest flash encryption.
   * Disabling CONFIG_GW_REQUIRE_FLASH_ENCRYPTION is bench-only. */
#if CONFIG_GW_REQUIRE_FLASH_ENCRYPTION
  if (!partition->encrypted || !esp_flash_encryption_enabled())
    return ESP_ERR_INVALID_STATE;
#endif
  lock = xSemaphoreCreateMutex();
  upload_lock = xSemaphoreCreateMutex();
  if (!lock || !upload_lock)
    return ESP_ERR_NO_MEM;
  esp_err_t restored = restore_upload();
  if (restored != ESP_OK)
    return restored;
  esp_fill_random(esp_boot, sizeof(esp_boot));
  gw_spool_io_t io = {.ctx = (void *)partition,
                      .read = io_read,
                      .write = io_write,
                      .erase = io_erase,
                      .erased = io_erased};
  int rc = gw_spool_open(&spool, &io, esp_boot);
  if (rc)
    return result(rc);
  rc = finalize(true);
  if (rc)
    return result(rc);
  initialized = true;
  if (xTaskCreate(timer_task, "capture_finalize", 8192, NULL, 5, NULL) !=
          pdPASS ||
      xTaskCreate(probe_task, "capture_clock", 6144, NULL, 4, NULL) != pdPASS) {
    initialized = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
uint8_t gw_capture_nrf(const uint8_t boot[16], const struct gw_frame *frame,
                       void *context) {
  (void)context;
  if (!initialized || !boot || !frame)
    return 2;
  if (frame->type < 0x20 || frame->type > 0x22 || frame->payload_len < 10 ||
      frame->payload_len > GW_BATCH_MAX)
    return 2;
  gw_spool_watermark_t w = {.batch = frame->msg_id,
                            .first = gw_get_u64(frame->payload + 2),
                            .count = gw_get_u16(frame->payload),
                            .type = frame->type};
  memcpy(w.boot, boot, 16);
  mbedtls_sha256(frame->payload, frame->payload_len, w.hash, 0);
  xSemaphoreTake(lock, portMAX_DELAY);
  int rc = gw_spool_check(&spool, &w);
  if (rc != GW_SP_OK) {
    xSemaphoreGive(lock);
    return rc == GW_SP_REPLAY ? 0 : rc == GW_SP_OLD ? 255 : 2;
  }
  if (frame->type == 0x22) {
    unsigned inbox = 0;
    for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
      if (gw_spool_state(&spool, i) == GW_SP_INBOX)
        inbox++;
    if (inbox + w.count > 32) {
      xSemaphoreGive(lock);
      return 1;
    }
  }
  gw_spool_record_t *records = calloc(w.count, sizeof(*records));
  if (!records) {
    xSemaphoreGive(lock);
    return 1;
  }
  size_t at = 10;
  uint64_t now = mono();
  for (unsigned i = 0; i < w.count; i++) {
    if (at + 24 > frame->payload_len) {
      rc = GW_SP_FAULT;
      break;
    }
    size_t n = 24u + gw_get_u16(frame->payload + at + 22);
    if (n > GW_SPOOL_RECORD_MAX || at + n > frame->payload_len ||
        !gw_capture_record_valid(frame->type, frame->payload + at, n)) {
      rc = GW_SP_FAULT;
      break;
    }
    records[i].type = frame->type;
    records[i].length = (uint16_t)n;
    memcpy(records[i].source_boot, boot, 16);
    memcpy(records[i].bytes, frame->payload + at, n);
    stamp(&records[i], now);
    at += n;
  }
  if (at != frame->payload_len)
    rc = GW_SP_FAULT;
  if (!rc && frame->type != 0x22)
    rc = pending_room(frame->payload_len - 10);
  if (!rc)
    rc = gw_spool_add(&spool, records, w.count, &w);
  if (rc == GW_SP_OK) {
    accepted_nrf += w.count;
    refresh();
  } else if (rc == GW_SP_FAULT)
    spool.fault = true;
  free(records);
  xSemaphoreGive(lock);
  return rc == GW_SP_OK ? 0 : rc == GW_SP_FULL ? 1 : 2;
}
static esp_err_t local_record(uint8_t type, const uint8_t boot[16],
                              const uint8_t *bytes, size_t length) {
  if (!initialized)
    return ESP_ERR_INVALID_STATE;
  if (!boot || !gw_capture_record_valid(type, bytes, length))
    return ESP_ERR_INVALID_ARG;
  gw_spool_record_t *v = calloc(1, sizeof(*v));
  if (!v)
    return ESP_ERR_NO_MEM;
  v->type = type;
  v->radio = 1;
  v->length = (uint16_t)length;
  memcpy(v->source_boot, boot, 16);
  memcpy(v->bytes, bytes, length);
  xSemaphoreTake(lock, portMAX_DELAY);
  stamp(v, mono());
  int rc = pending_room(length);
  if (!rc)
    rc = gw_spool_add(&spool, v, 1, NULL);
  if (!rc) {
    accepted_esp++;
    refresh();
  }
  xSemaphoreGive(lock);
  free(v);
  return result(rc);
}
esp_err_t gw_capture_esp(const uint8_t *record, size_t length) {
  return local_record(0x20, esp_boot, record, length);
}
esp_err_t gw_capture_esp_gatt(const uint8_t boot[16], const uint8_t *record,
                              size_t length) {
  if (!boot || memcmp(boot, esp_boot, 16))
    return ESP_ERR_INVALID_ARG;
  return local_record(0x21, boot, record, length);
}
size_t gw_capture_used_bytes(void) {
  if (!lock)
    return 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  size_t n = gw_spool_used(&spool);
  xSemaphoreGive(lock);
  return n;
}
static char *base64(const uint8_t *p, size_t n) {
  size_t capacity = 4 * ((n + 2) / 3) + 1, used = 0;
  char *out = malloc(capacity);
  if (!out)
    return NULL;
  if (mbedtls_base64_encode((uint8_t *)out, capacity, &used, p, n)) {
    free(out);
    return NULL;
  }
  out[used] = 0;
  return out;
}
static cJSON *make_event(unsigned slot, const gw_spool_record_t *v) {
  cJSON *event = cJSON_CreateObject(), *capture = cJSON_CreateObject();
  if (!event || !capture) {
    cJSON_Delete(event);
    cJSON_Delete(capture);
    return NULL;
  }
  const uint8_t *p = v->bytes;
  char id[80];
  event_id(slot, v, id);
  char *encoded = base64(p + 24, v->length - 24);
  if (!encoded) {
    cJSON_Delete(event);
    cJSON_Delete(capture);
    return NULL;
  }
  cJSON_AddStringToObject(event, "eventId", id);
  cJSON_AddStringToObject(event, "payloadBase64", encoded);
  free(encoded);
  cJSON_AddStringToObject(event, "timeQuality",
                          v->utc_ms && v->type != 0x23 ? "SYNCED" : "UNSYNCED");
  if (v->type != 0x23)
    cJSON_AddNumberToObject(event, "sequence",
                            (double)gw_spool_sequence(&spool, slot));
  if (v->utc_ms && v->type != 0x23) {
    time_t seconds = (time_t)(v->utc_ms / 1000);
    struct tm tm;
    char formatted[40], seconds_text[24];
    if (!gmtime_r(&seconds, &tm) ||
        !strftime(seconds_text, sizeof(seconds_text), "%Y-%m-%dT%H:%M:%S",
                  &tm)) {
      cJSON_Delete(event);
      cJSON_Delete(capture);
      return NULL;
    }
    snprintf(formatted, sizeof(formatted), "%s.%03uZ", seconds_text,
             (unsigned)(v->utc_ms % 1000));
    cJSON_AddStringToObject(event, "capturedAt", formatted);
  }
  cJSON_AddStringToObject(capture, "radio", v->radio ? "ESP32" : "NRF54");
  unsigned address_at = v->type == 0x20 ? 10 : 12;
  if (v->type != 0x23 && p[address_at] != 255) {
    char address[13], source[32];
    hex(p + address_at + 1, 6, address, true);
    cJSON_AddStringToObject(capture, "addressType",
                            p[address_at] ? "RANDOM" : "PUBLIC");
    cJSON_AddStringToObject(capture, "address", address);
    snprintf(source, sizeof(source), "ble:%s:%s",
             p[address_at] ? "random" : "public", address);
    cJSON_AddStringToObject(event, "sourceDeviceId", source);
  }
  if (v->type == 0x20) {
    cJSON_AddStringToObject(event, "protocolHint", "ble-ad-v1");
    cJSON_AddStringToObject(capture, "kind", p[8] & 1 ? "SCAN_RSP" : "ADV");
    cJSON_AddBoolToObject(capture, "legacy", p[8] < 2);
    if (!(p[8] & 1))
      cJSON_AddBoolToObject(capture, "connectable", p[9] & 1);
    if ((int8_t)p[17] != -128 && p[17] != 127)
      cJSON_AddNumberToObject(capture, "rssiDbm", (int8_t)p[17]);
    if ((int8_t)p[18] != -128 && p[18] != 127)
      cJSON_AddNumberToObject(capture, "txPowerDbm", (int8_t)p[18]);
    const char *phys[] = {"", "LE_1M", "LE_2M", "LE_CODED"};
    if (p[19])
      cJSON_AddStringToObject(capture, "primaryPhy", phys[p[19]]);
    if (p[8] >= 2 && p[20])
      cJSON_AddStringToObject(capture, "secondaryPhy", phys[p[20]]);
    if (p[8] >= 2 && p[21] != 255)
      cJSON_AddNumberToObject(capture, "sid", p[21]);
    if (gw_spool_merged(&spool, slot)) {
      cJSON *radios_json = cJSON_AddArrayToObject(capture, "observedBy");
      cJSON_AddItemToArray(radios_json, cJSON_CreateString("NRF54"));
      cJSON_AddItemToArray(radios_json, cJSON_CreateString("ESP32"));
    }
  } else {
    char ref[33];
    gw_capture_connection_ref(v->radio, v->source_boot, gw_get_u32(p + 8), ref);
    cJSON_AddStringToObject(capture, "connectionRef", ref);
    if (v->type == 0x23) {
      cJSON_AddStringToObject(event, "protocolHint", "ble-gatt-link-v1");
      cJSON_AddStringToObject(capture, "kind", "GATT_LINK_DOWN");
      cJSON_AddStringToObject(capture, "linkReason", reasons[p[21]]);
    } else {
      cJSON_AddStringToObject(event, "protocolHint", "ble-gatt-value-v1");
      cJSON_AddStringToObject(capture, "kind",
                              p[21] ? "GATT_INDICATE" : "GATT_NOTIFY");
      cJSON_AddNumberToObject(capture, "characteristicHandle",
                              gw_get_u16(p + 19));
    }
  }
  cJSON_AddItemToObject(event, "capture", capture);
  return event;
}
esp_err_t gw_capture_upload(const char *namespace_id, const char *gateway_id,
                            uint64_t deadline) {
  if (!initialized || !namespace_id || !gateway_id)
    return ESP_ERR_INVALID_ARG;
  uint64_t now = mono();
  if (now >= deadline)
    return ESP_ERR_TIMEOUT;
  if (xSemaphoreTake(upload_lock, pdMS_TO_TICKS(deadline - now) + 1) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (spool.fault || now < next_upload) {
    xSemaphoreGive(lock);
    xSemaphoreGive(upload_lock);
    return spool.fault ? ESP_FAIL : ESP_ERR_TIMEOUT;
  }
  int rc = finalize(false);
  if (rc) {
    xSemaphoreGive(lock);
    xSemaphoreGive(upload_lock);
    return result(rc);
  }
  unsigned ready_count = 0;
  uint64_t oldest_arrival = UINT64_MAX;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) == GW_SP_READY) {
      ready_count++;
      if (pending[i].arrived < oldest_arrival)
        oldest_arrival = pending[i].arrived;
    }
  if (!ready_count ||
      (ready_count < upload_batch_max && now >= oldest_arrival &&
       now - oldest_arrival < upload_flush_ms)) {
    xSemaphoreGive(lock);
    xSemaphoreGive(upload_lock);
    return ESP_OK;
  }
  unsigned batch_limit = upload_batch_max > 200 ? 200 : upload_batch_max;
  typedef struct {
    uint16_t slot;
    char id[80];
  } upload_item_t;
  upload_item_t *sent = calloc(batch_limit, sizeof(*sent));
  size_t count = 0;
  uint64_t oldest = UINT64_MAX;
  cJSON *body = cJSON_CreateObject(), *events = cJSON_CreateArray();
  gw_spool_record_t *record = malloc(sizeof(*record));
  if (!body || !events || !record || !sent) {
    cJSON_Delete(body);
    cJSON_Delete(events);
    free(record);
    free(sent);
    xSemaphoreGive(lock);
    xSemaphoreGive(upload_lock);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(body, "events", events);
  cJSON_AddStringToObject(body, "schemaVersion", "1.1");
  cJSON_AddStringToObject(body, "namespaceId", namespace_id);
  cJSON_AddStringToObject(body, "gatewayId", gateway_id);
  /* Oldest events first, independent of physical slot wrap. */
  for (size_t k = 0; k < batch_limit; k++) {
    unsigned chosen = GW_SPOOL_NONE;
    oldest = UINT64_MAX;
    for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
      if (gw_spool_state(&spool, i) == GW_SP_READY) {
        bool seen = false;
        for (size_t j = 0; j < count; j++)
          if (sent[j].slot == i)
            seen = true;
        uint64_t sequence = gw_spool_sequence(&spool, i);
        if (!seen && sequence < oldest) {
          chosen = i;
          oldest = sequence;
        }
      }
    if (chosen == GW_SPOOL_NONE)
      break;
    rc = gw_spool_read(&spool, chosen, record);
    if (rc)
      break;
    cJSON *event = make_event(chosen, record);
    if (!event) {
      rc = GW_SP_FULL;
      break;
    }
    event_id(chosen, record, sent[count].id);
    sent[count++].slot = (uint16_t)chosen;
    cJSON_AddItemToArray(events, event);
  }
  char batch_id[49];
  uint8_t random[16];
  esp_fill_random(random, 16);
  memcpy(batch_id, "raw-", 4);
  hex(random, 16, batch_id + 4, false);
  cJSON_AddStringToObject(body, "batchId", batch_id);
  free(record);
  xSemaphoreGive(lock);
  if (rc || !count) {
    cJSON_Delete(body);
    free(sent);
    xSemaphoreGive(upload_lock);
    return rc ? result(rc) : ESP_OK;
  }
  cJSON *ack = NULL;
  esp_err_t e = gw_transport_json("/api/v1/gateway-ingress/batches", "POST", body,
                             NULL, deadline, &ack);
  cJSON_Delete(body);
  bool valid = e == ESP_OK && !strcmp(str(ack, "namespaceId"), namespace_id) &&
               !strcmp(str(ack, "gatewayId"), gateway_id) &&
               !strcmp(str(ack, "batchId"), batch_id) &&
               !strcmp(str(ack, "schemaVersion"), "1.1") &&
               !strcmp(str(ack, "durability"), "RAW_PERSISTED");
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(ack, "items");
  valid = valid && cJSON_IsArray(items);
  uint16_t changed[200];
  uint8_t states[200];
  size_t changes = 0;
  uint64_t delay = 250;
  if (valid) {
    for (size_t i = 0; i < count; i++) {
      const cJSON *matched = NULL;
      unsigned matches = 0;
      for (const cJSON *item = items->child; item; item = item->next)
        if (!strcmp(str(item, "eventId"), sent[i].id)) {
          matched = item;
          matches++;
        }
      if (matches != 1)
        continue;
      const char *status = str(matched, "status");
      if (!strcmp(status, "ACCEPTED") || !strcmp(status, "DUPLICATE")) {
        changed[changes] = sent[i].slot;
        states[changes++] = GW_SP_FREE;
      } else if (!strcmp(status, "REJECTED_PERMANENT")) {
        changed[changes] = sent[i].slot;
        states[changes++] = GW_SP_QUARANTINE;
      } else if (!strcmp(status, "REJECTED_RETRYABLE")) {
        const cJSON *retry =
            cJSON_GetObjectItemCaseSensitive(matched, "retryAfterMs");
        if (cJSON_IsNumber(retry) && retry->valuedouble > delay) {
          delay = retry->valuedouble > 4294967295.0
                      ? UINT32_MAX
                      : (uint64_t)retry->valuedouble;
        }
      }
    }
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (valid) {
    rc = gw_spool_mark(&spool, changed, states, changes);
    if (!rc)
      refresh();
    upload_failures = 0;
    if (changes == count)
      delay = 0;
  } else {
    if (upload_failures < 7)
      upload_failures++;
    delay = 250u << upload_failures;
    if (delay > 30000)
      delay = 30000;
    e = e == ESP_OK ? ESP_ERR_INVALID_RESPONSE : e;
  }
  next_upload = mono() + delay + (delay ? esp_random() % 101u : 0);
  xSemaphoreGive(lock);
  cJSON_Delete(ack);
  free(sent);
  xSemaphoreGive(upload_lock);
  return rc ? result(rc) : e;
}
bool gw_capture_adv_evidence(const char *id, const char *address,
                             const char *type, const uint8_t boot[16],
                             uint32_t age) {
  if (!initialized || !id || !address || !type || !boot || !age ||
      !gw_core_time_trusted())
    return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  uint64_t now = wall_ms();
  bool okay = false;
  for (unsigned i = 0; i < 64; i++) {
    const evidence_t *e = &evidence[i];
    if (!strcmp(e->event, id) && !strcmp(e->address, address) &&
        !strcmp(type, e->address_type ? "RANDOM" : "PUBLIC") &&
        !memcmp(e->boot, boot, 16) && now >= e->utc_ms &&
        now - e->utc_ms <= age) {
      okay = true;
      break;
    }
  }
  xSemaphoreGive(lock);
  return okay;
}
esp_err_t gw_capture_link_down(uint8_t radio, const uint8_t boot[16],
                               uint32_t connection, uint8_t type,
                               const uint8_t address[6], const char *reason) {
  if (!initialized || radio > 1 || !boot || !address || !reason || !connection)
    return ESP_ERR_INVALID_ARG;
  unsigned reason_code = 3;
  for (unsigned i = 0; i < 3; i++)
    if (!strcmp(reason, reasons[i]))
      reason_code = i;
  if (reason_code == 3)
    return ESP_ERR_INVALID_ARG;
  gw_spool_record_t *v = calloc(1, sizeof(*v)), *old = malloc(sizeof(*old));
  if (!v || !old) {
    free(v);
    free(old);
    return ESP_ERR_NO_MEM;
  }
  v->type = 0x23;
  v->radio = radio;
  v->length = 24;
  memcpy(v->source_boot, boot, 16);
  gw_put_u32(v->bytes + 8, connection);
  v->bytes[12] = type;
  memcpy(v->bytes + 13, address, 6);
  v->bytes[21] = (uint8_t)reason_code;
  if (!gw_capture_record_valid(0x23, v->bytes, 24)) {
    free(v);
    free(old);
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  int rc = GW_SP_OK;
  bool exists = false;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) && pending[i].type == 0x23) {
      rc = gw_spool_read(&spool, i, old);
      if (rc)
        break;
      if (old->radio == radio && !memcmp(old->source_boot, boot, 16) &&
          gw_get_u32(old->bytes + 8) == connection &&
          old->bytes[21] == reason_code) {
        exists = true;
        break;
      }
    }
  if (!rc && !exists) {
    v->arrived_ms = mono();
    rc = gw_spool_add(&spool, v, 1, NULL);
    if (!rc)
      refresh();
  }
  xSemaphoreGive(lock);
  free(v);
  free(old);
  return result(rc);
}
static void inbox_token(const gw_spool_record_t *v, char out[80]) {
  char boot[33];
  hex(v->source_boot, 16, boot, false);
  snprintf(out, 80, "inbox-%s-%" PRIu64, boot, v->order);
}
cJSON *gw_capture_peripheral_peek(void) {
  if (!initialized)
    return NULL;
  xSemaphoreTake(lock, portMAX_DELAY);
  unsigned slot = GW_SPOOL_NONE;
  uint64_t order = UINT64_MAX;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(&spool, i) == GW_SP_INBOX && pending[i].order < order) {
      slot = i;
      order = pending[i].order;
    }
  cJSON *out = NULL;
  gw_spool_record_t *v = malloc(sizeof(*v));
  if (slot != GW_SPOOL_NONE && v &&
      gw_spool_read(&spool, slot, v) == GW_SP_OK) {
    char token[80], ref[33], boot[33];
    inbox_token(v, token);
    gw_capture_connection_ref(v->radio, v->source_boot,
                              gw_get_u32(v->bytes + 8), ref);
    char *encoded = base64(v->bytes + 24, v->length - 24);
    out = cJSON_CreateObject();
    if (out && encoded) {
      cJSON_AddStringToObject(out, "receiptToken", token);
      cJSON_AddStringToObject(out, "connectionRef", ref);
      hex(v->source_boot, 16, boot, false);
      cJSON_AddStringToObject(out, "radio", v->radio ? "ESP32" : "NRF54");
      cJSON_AddStringToObject(out, "controllerBootId", boot);
      cJSON_AddNumberToObject(out, "connectionId", gw_get_u32(v->bytes + 8));
      cJSON_AddNumberToObject(out, "captureMs", (double)gw_get_u64(v->bytes));
      cJSON_AddNumberToObject(out, "arrivedMs", (double)v->arrived_ms);
      hex(gw_spool_event_boot(&spool, slot), 16, boot, false);
      cJSON_AddStringToObject(out, "ingressBootId", boot);
      if (v->utc_ms)
        cJSON_AddNumberToObject(out, "receivedUtcMs", (double)v->utc_ms);
      cJSON_AddStringToObject(out, "payloadBase64", encoded);
      cJSON_AddStringToObject(out, "trust", "UNAUTHENTICATED");
    } else {
      cJSON_Delete(out);
      out = NULL;
    }
    free(encoded);
  }
  free(v);
  xSemaphoreGive(lock);
  return out;
}
esp_err_t gw_capture_peripheral_ack(const char *token) {
  if (!initialized || !token)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(lock, portMAX_DELAY);
  gw_spool_record_t *v = malloc(sizeof(*v));
  esp_err_t e = v ? ESP_ERR_NOT_FOUND : ESP_ERR_NO_MEM;
  if (v)
    for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
      if (gw_spool_state(&spool, i) == GW_SP_INBOX) {
        int rc = gw_spool_read(&spool, i, v);
        if (rc) {
          e = result(rc);
          break;
        }
        char actual[80];
        inbox_token(v, actual);
        if (!strcmp(actual, token)) {
          uint16_t slot = (uint16_t)i;
          uint8_t state = GW_SP_FREE;
          e = result(gw_spool_mark(&spool, &slot, &state, 1));
          if (e == ESP_OK)
            refresh();
          break;
        }
      }
  free(v);
  xSemaphoreGive(lock);
  return e;
}
cJSON *gw_capture_diagnostics(void) {
  if (!lock)
    return NULL;
  xSemaphoreTake(lock, portMAX_DELAY);
  cJSON *o = cJSON_CreateObject();
  unsigned states[5] = {0};
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++) {
    unsigned state = gw_spool_state(&spool, i);
    if (state < 5)
      states[state]++;
  }
  if (o) {
    cJSON_AddBoolToObject(o, "storageFault", spool.fault);
    cJSON_AddNumberToObject(o, "usedBytes", (double)gw_spool_used(&spool));
    cJSON_AddNumberToObject(o, "capacityRecords", GW_SPOOL_SLOTS);
    cJSON_AddNumberToObject(o, "pending", states[GW_SP_PENDING]);
    cJSON_AddNumberToObject(o, "ready", states[GW_SP_READY]);
    cJSON_AddNumberToObject(o, "quarantined", states[GW_SP_QUARANTINE]);
    cJSON_AddNumberToObject(o, "peripheralInbox", states[GW_SP_INBOX]);
    cJSON_AddNumberToObject(o, "acceptedNrf", (double)accepted_nrf);
    cJSON_AddNumberToObject(o, "acceptedEsp", (double)accepted_esp);
    cJSON_AddNumberToObject(o, "merged", (double)merged_count);
    cJSON_AddNumberToObject(o, "unmapped", (double)unmapped_count);
    cJSON_AddNumberToObject(o, "recoveredUnpaired", (double)recovered_count);
  }
  xSemaphoreGive(lock);
  return o;
}
