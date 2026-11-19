#include "freertos/FreeRTOS.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_http_private.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include <stdlib.h>
#include <string.h>
static portMUX_TYPE ca_lock = portMUX_INITIALIZER_UNLOCKED;
static bool ready, busy;
static mbedtls_x509_crt roots;
static uint64_t epoch(const mbedtls_x509_time *t) {
  int y = t->year, m = t->mon;
  y -= m <= 2;
  int era = y / 400;
  unsigned yo = y - era * 400, mp = m + (m > 2 ? -3 : 9),
           day = (153 * mp + 2) / 5 + t->day - 1;
  int64_t days =
      (int64_t)era * 146097 + yo * 365 + yo / 4 - yo / 100 + day - 719468;
  if (days < 0)
    return 0;
  return ((uint64_t)days * 86400 + t->hour * 3600 + t->min * 60 + t->sec) *
         1000;
}
esp_err_t gw_http_tls_prepare(void) {
  portENTER_CRITICAL(&ca_lock);
  if (ready || busy) {
    bool done = ready;
    portEXIT_CRITICAL(&ca_lock);
    return done ? ESP_OK : ESP_ERR_NOT_FINISHED;
  }
  busy = true;
  portEXIT_CRITICAL(&ca_lock);
  char *pem = calloc(1, 8192);
  esp_err_t e = ESP_ERR_NO_MEM;
  if (pem) {
    e = gw_factory_get_string("root_ca", pem, 8192);
    mbedtls_x509_crt_init(&roots);
    if (e == ESP_OK &&
        mbedtls_x509_crt_parse(&roots, (uint8_t *)pem, strlen(pem) + 1))
      e = ESP_ERR_INVALID_RESPONSE;
    free(pem);
  }
  if (e != ESP_OK)
    mbedtls_x509_crt_free(&roots);
  portENTER_CRITICAL(&ca_lock);
  ready = e == ESP_OK;
  busy = false;
  portEXIT_CRITICAL(&ca_lock);
  return e;
}
static int verify_interval(void *ctx, mbedtls_x509_crt *cert, int depth,
                           uint32_t *flags) {
  (void)ctx;
  (void)depth;
  uint64_t lower, upper;
  if (!gw_core_time_bounds(&lower, &upper))
    *flags |= MBEDTLS_X509_BADCERT_OTHER;
  else {
    if (lower < epoch(&cert->valid_from))
      *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (upper > epoch(&cert->valid_to))
      *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
  }
  return 0;
}
esp_err_t gw_http_tls_attach(void *configuration) {
  if (!configuration || !ready)
    return ESP_ERR_INVALID_STATE;
  mbedtls_ssl_config *cfg = configuration;
  mbedtls_ssl_conf_authmode(cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(cfg, &roots, NULL);
  mbedtls_ssl_conf_verify(cfg, verify_interval, NULL);
  return ESP_OK;
}
esp_err_t gw_http_tls_interval(esp_tls_t *tls) {
  uint64_t lower, upper;
  if (!gw_core_time_bounds(&lower, &upper))
    return ESP_ERR_INVALID_STATE;
  mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(tls);
  const mbedtls_x509_crt *cert = ssl ? mbedtls_ssl_get_peer_cert(ssl) : NULL;
  if (!cert)
    return ESP_ERR_INVALID_RESPONSE;
  for (; cert; cert = cert->next)
    if (lower < epoch(&cert->valid_from) || upper > epoch(&cert->valid_to))
      return ESP_ERR_INVALID_RESPONSE;
  return ESP_OK;
}
