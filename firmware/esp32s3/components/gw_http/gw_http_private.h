#pragma once
#include "esp_netif.h"
#include "esp_tls.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
esp_err_t gw_http_resolve(esp_netif_t *,const char *,struct in_addr *,struct ifreq *,uint64_t);
esp_err_t gw_http_tls_interval(esp_tls_t *tls);
