#pragma once
#include "esp_err.h"
#include <stdint.h>
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
typedef struct {
  unsigned id;
} esp_netif_t;
typedef struct {
  struct {
    uint32_t addr;
  } ip;
} esp_netif_ip_info_t;
typedef struct {
  struct {
    int type;
    union {
      struct {
        uint32_t addr;
      } ip4;
    } u_addr;
  } ip;
} esp_netif_dns_info_t;
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *, char *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_netif_get_dns_info(esp_netif_t *, int, esp_netif_dns_info_t *);
esp_netif_t *esp_netif_get_default_netif(void);
