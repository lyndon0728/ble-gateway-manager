#pragma once
#include "lwip/sockets.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#define ESP_TLS_ERR_SSL_WANT_READ -0x6900
#define ESP_TLS_ERR_SSL_WANT_WRITE -0x6880
typedef struct {
  bool alive;
} esp_tls_t;
typedef enum {
  ESP_TLS_INIT,
  ESP_TLS_CONNECTING,
  ESP_TLS_HANDSHAKE,
  ESP_TLS_FAIL,
  ESP_TLS_DONE
} esp_tls_conn_state_t;
int esp_tls_get_conn_state(esp_tls_t *, esp_tls_conn_state_t *);
typedef struct {
  const unsigned char *cacert_buf;
  int (*crt_bundle_attach)(void *);
  unsigned cacert_bytes;
  const char *common_name;
  bool skip_common_name, non_block;
  int timeout_ms;
  struct ifreq *if_name;
} esp_tls_cfg_t;
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *,
                           esp_tls_t *);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
int esp_tls_conn_destroy(esp_tls_t *);
