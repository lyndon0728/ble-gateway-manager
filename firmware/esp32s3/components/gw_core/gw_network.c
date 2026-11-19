#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_eth_mac_ch390.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy_ch390.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "gw_internal.h"
#include "gw_rules.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static esp_netif_t *sta_netif, *ap_netif, *eth_netif;
static esp_eth_handle_t eth_handle;
static bool ap_open, sta_wanted;
static atomic_bool sta_connected, sta_connecting, eth_connected, sta_has_ip,
    eth_has_ip;
static unsigned retry_count;
static uint64_t reconnect_at;
static SemaphoreHandle_t wifi_lock;
static SemaphoreHandle_t scan_done;
static bool scan_running, scan_fault;
static uint32_t scan_status;
static atomic_bool clock_synced;
static atomic_bool ap_route_fault;
static char uplinks[2][16];
static unsigned uplink_count;

uint64_t gw_core_mono_ms(void) {
  return (uint64_t)esp_timer_get_time() / 1000u;
}
static void clock_notification(struct timeval *tv) {
  clock_synced = tv && tv->tv_sec >= 1735689600;
}
bool gw_core_time_trusted(void) {
  return clock_synced && time(NULL) >= 1735689600;
}
static bool subnet_overlap(const esp_netif_ip_info_t *a,
                           const esp_netif_ip_info_t *b) {
  uint32_t mask = a->netmask.addr & b->netmask.addr;
  return a->ip.addr && b->ip.addr && (a->ip.addr & mask) == (b->ip.addr & mask);
}
static esp_err_t ap_subnet(const cJSON *candidate) {
  if (!ap_open)
    return ESP_OK;
  ap_route_fault = true;
  const char *choices[] = {"192.168.4.1", "192.168.50.1", "192.168.87.1",
                           "172.31.254.1"};
  esp_netif_ip_info_t known[4] = {0};
  esp_netif_get_ip_info(sta_netif, &known[0]);
  if (eth_netif)
    esp_netif_get_ip_info(eth_netif, &known[1]);
  const char *groups[] = {"wifiSta", "ethernet"};
  for (unsigned i = 0; i < 2 && candidate; i++) {
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(candidate, groups[i]),
                *ip = cJSON_GetObjectItemCaseSensitive(g, "ipv4");
    const char *mode = gw_json_string(ip, "mode");
    const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(ip, "prefixLength");
    const char *address = gw_json_string(ip, "address");
    if (mode && !strcmp(mode, "STATIC") && address && cJSON_IsNumber(prefix)) {
      esp_netif_str_to_ip4(address, &known[i + 2].ip);
      known[i + 2].netmask.addr = htonl(UINT32_MAX << (32 - prefix->valueint));
    }
  }
  for (unsigned i = 0; i < 4; i++) {
    esp_netif_ip_info_t next = {0};
    esp_netif_str_to_ip4(choices[i], &next.ip);
    next.gw = next.ip;
    next.netmask.addr = htonl(0xffffff00u);
    bool conflict = false;
    for (unsigned j = 0; j < 4; j++)
      if (subnet_overlap(&next, &known[j]))
        conflict = true;
    if (conflict)
      continue;
    esp_netif_ip_info_t current;
    esp_netif_get_ip_info(ap_netif, &current);
    if (current.ip.addr == next.ip.addr) {
      ap_route_fault = false;
      return ESP_OK;
    }
    esp_err_t e = esp_netif_dhcps_stop(ap_netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
      return e;
    e = esp_netif_set_ip_info(ap_netif, &next);
    if (e == ESP_OK)
      e = esp_netif_dhcps_start(ap_netif);
    if (e == ESP_OK) {
      esp_wifi_deauth_sta(0);
      ap_route_fault = false;
    }
    return e;
  }
  ap_route_fault = true;
  return ESP_ERR_INVALID_STATE;
}
static void network_event(void *arg, esp_event_base_t base, int32_t id,
                          void *event) {
  (void)arg;
  (void)event;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE && scan_running) {
    const wifi_event_sta_scan_done_t *done = event;
    scan_status = done ? done->status : 1;
    xSemaphoreGive(scan_done);
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    sta_connected = true;
    sta_connecting = false;
    retry_count = 0;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    sta_connected = false;
    sta_has_ip = false;
    sta_connecting = false;
    reconnect_at = gw_core_mono_ms() +
                   ((1u << (retry_count < 5 ? retry_count++ : 5)) * 1000u);
  }
  if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED)
    eth_connected = true;
  if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
    eth_connected = false;
    eth_has_ip = false;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    sta_has_ip = true;
  if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP)
    eth_has_ip = true;
  if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)
    sta_has_ip = false;
  if (base == IP_EVENT &&
      (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP))
    ap_subnet(NULL);
  xSemaphoreGive(wifi_lock);
}
static void retry_task(void *unused) {
  (void)unused;
  for (;;) {
    xSemaphoreTake(wifi_lock, portMAX_DELAY);
    if (sta_wanted && !sta_connected && !sta_connecting && !scan_running &&
        gw_core_mono_ms() >= reconnect_at) {
      sta_connecting = esp_wifi_connect() == ESP_OK;
      reconnect_at = gw_core_mono_ms() + 30000;
    }
    for (unsigned i = 0; i < uplink_count; i++)
      if (gw_core_interface_ready(uplinks[i])) {
        esp_netif_set_default_netif(gw_core_uplink_netif(uplinks[i]));
        break;
      }
    xSemaphoreGive(wifi_lock);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
static esp_err_t set_ipv4(esp_netif_t *netif, const cJSON *ip) {
  if (!netif || !cJSON_IsObject(ip))
    return ESP_ERR_INVALID_ARG;
  const char *mode = gw_json_string(ip, "mode");
  if (mode && !strcmp(mode, "DHCP")) {
    esp_err_t e = esp_netif_dhcpc_start(netif);
    return e == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : e;
  }
  esp_err_t err = esp_netif_dhcpc_stop(netif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    return err;
  esp_netif_ip_info_t info = {0};
  const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(ip, "prefixLength");
  const char *a = gw_json_string(ip, "address"),
             *g = gw_json_string(ip, "gateway");
  if (!a || !g || !cJSON_IsNumber(prefix) ||
      esp_netif_str_to_ip4(a, &info.ip) != ESP_OK ||
      esp_netif_str_to_ip4(g, &info.gw) != ESP_OK)
    return ESP_ERR_INVALID_ARG;
  info.netmask.addr = htonl(UINT32_MAX << (32 - prefix->valueint));
  err = esp_netif_set_ip_info(netif, &info);
  const cJSON *dns = cJSON_GetObjectItemCaseSensitive(ip, "dns");
  for (int i = 0; i < cJSON_GetArraySize(dns) && err == ESP_OK; i++) {
    esp_netif_dns_info_t d = {0};
    d.ip.type = ESP_IPADDR_TYPE_V4;
    if (esp_netif_str_to_ip4(cJSON_GetArrayItem(dns, i)->valuestring,
                             &d.ip.u_addr.ip4) != ESP_OK)
      return ESP_ERR_INVALID_ARG;
    err = esp_netif_set_dns_info(
        netif, i == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d);
  }
  return err;
}

esp_err_t gw_network_init(void) {
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK)
    return e;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
    return e;
  wifi_lock = xSemaphoreCreateMutex();
  scan_done = xSemaphoreCreateBinary();
  if (!wifi_lock || !scan_done)
    return ESP_ERR_NO_MEM;
  sta_netif = esp_netif_create_default_wifi_sta();
  ap_netif = esp_netif_create_default_wifi_ap();
  if (!sta_netif || !ap_netif)
    return ESP_ERR_NO_MEM;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  init.nvs_enable = 0;
  e = esp_wifi_init(&init);
  if (e != ESP_OK)
    return e;
  e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (e != ESP_OK)
    return e;
  e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event,
                                 NULL);
  if (e != ESP_OK)
    return e;
  e = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event,
                                 NULL);
  if (e != ESP_OK)
    return e;
  e = esp_wifi_set_mode(WIFI_MODE_STA);
  if (e == ESP_OK)
    e = esp_wifi_start();
  if (e != ESP_OK)
    return e;
#if CONFIG_GW_ETH_ENABLED
  if (CONFIG_GW_ETH_MOSI < 0 || CONFIG_GW_ETH_MISO < 0 ||
      CONFIG_GW_ETH_SCLK < 0 || CONFIG_GW_ETH_CS < 0)
    return ESP_ERR_INVALID_STATE;
  spi_bus_config_t bus = {.mosi_io_num = CONFIG_GW_ETH_MOSI,
                          .miso_io_num = CONFIG_GW_ETH_MISO,
                          .sclk_io_num = CONFIG_GW_ETH_SCLK,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1};
  e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (e != ESP_OK)
    return e;
  spi_device_interface_config_t dev = {.mode = 0,
                                       .clock_speed_hz = 20000000,
                                       .queue_size = 16,
                                       .spics_io_num = CONFIG_GW_ETH_CS};
  eth_ch390_config_t chip = ETH_CH390_DEFAULT_CONFIG(SPI2_HOST, &dev);
  chip.int_gpio_num = CONFIG_GW_ETH_INT;
  if (CONFIG_GW_ETH_INT < 0)
    chip.poll_period_ms = 20;
  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  mac_cfg.rx_task_stack_size = 4096;
  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.reset_gpio_num = CONFIG_GW_ETH_RESET;
  esp_eth_mac_t *mac = esp_eth_mac_new_ch390(&chip, &mac_cfg);
  esp_eth_phy_t *phy = esp_eth_phy_new_ch390(&phy_cfg);
  if (!mac || !phy)
    return ESP_ERR_NO_MEM;
  esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
  e = esp_eth_driver_install(&eth_cfg, &eth_handle);
  if (e != ESP_OK)
    return e;
  esp_netif_config_t eth_ip = ESP_NETIF_DEFAULT_ETH();
  eth_netif = esp_netif_new(&eth_ip);
  if (!eth_netif)
    return ESP_ERR_NO_MEM;
  e = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
  if (e != ESP_OK)
    return e;
  e = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, network_event,
                                 NULL);
  if (e != ESP_OK)
    return e;
#endif
  char ntp[128];
  if (gw_factory_get_string("ntp_server", ntp, sizeof(ntp)) == ESP_OK) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, strdup(ntp));
    esp_sntp_set_time_sync_notification_cb(clock_notification);
    esp_sntp_init();
  }
  return xTaskCreate(retry_task, "gw_net_retry", 3072, NULL, 4, NULL) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

esp_err_t gw_network_apply(const cJSON *network) {
  if (!network) {
    xSemaphoreTake(wifi_lock, portMAX_DELAY);
    sta_wanted = false;
    sta_has_ip = false;
    uplink_count = 0;
    esp_wifi_disconnect();
    xSemaphoreGive(wifi_lock);
    if (eth_handle) {
      eth_has_ip = false;
      esp_eth_stop(eth_handle);
    }
    return ESP_OK;
  }
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  esp_err_t subnet = ap_subnet(network);
  xSemaphoreGive(wifi_lock);
  if (subnet != ESP_OK)
    return subnet;
  const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(network, "wifiSta"),
              *eth = cJSON_GetObjectItemCaseSensitive(network, "ethernet");
  bool enable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wifi, "enabled"));
  wifi_config_t cfg = {0};
  if (enable) {
    const char *ssid = gw_json_string(wifi, "ssid"),
               *sec = gw_json_string(wifi, "security"),
               *ref = gw_json_string(wifi, "credentialRef");
    char password[65] = {0};
    esp_err_t e = gw_credential_password(ref, ssid, sec, password);
    if (e != ESP_OK)
      return e;
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, password, strlen(password));
    memset(password, 0, sizeof(password));
    cfg.sta.threshold.authmode =
        !strcmp(sec, "WPA3_SAE") ? WIFI_AUTH_WPA3_PSK : WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = !strcmp(sec, "WPA3_SAE");
  }
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  sta_wanted = false;
  sta_connecting = false;
  sta_connected = false;
  sta_has_ip = false;
  esp_wifi_disconnect();
  esp_err_t err = esp_wifi_set_mode(ap_open ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  if (enable && err == ESP_OK)
    err = set_ipv4(sta_netif, cJSON_GetObjectItemCaseSensitive(wifi, "ipv4"));
  if (enable && err == ESP_OK)
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  memset(&cfg, 0, sizeof(cfg));
  if (enable && err == ESP_OK) {
    sta_wanted = true;
    retry_count = 0;
    reconnect_at = gw_core_mono_ms();
  }
  xSemaphoreGive(wifi_lock);
  if (eth_netif) {
    eth_has_ip = false;
    esp_eth_stop(eth_handle);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eth, "enabled")) &&
        err == ESP_OK) {
      err = set_ipv4(eth_netif, cJSON_GetObjectItemCaseSensitive(eth, "ipv4"));
      if (err == ESP_OK)
        err = esp_eth_start(eth_handle);
    }
  }
  const cJSON *first = cJSON_GetArrayItem(
      cJSON_GetObjectItemCaseSensitive(network, "uplinkOrder"), 0);
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  uplink_count = 0;
  const cJSON *order = cJSON_GetObjectItemCaseSensitive(network, "uplinkOrder");
  for (const cJSON *u = order ? order->child : NULL; u && uplink_count < 2;
       u = u->next) {
    snprintf(uplinks[uplink_count++], sizeof(uplinks[0]), "%s", u->valuestring);
  }
  xSemaphoreGive(wifi_lock);
  if (cJSON_IsString(first)) {
    esp_netif_t *def = gw_core_uplink_netif(first->valuestring);
    if (def && err == ESP_OK)
      err = esp_netif_set_default_netif(def);
  }
  return err;
}

esp_netif_t *gw_core_ap_netif(void) { return ap_netif; }
esp_netif_t *gw_core_uplink_netif(const char *name) {
  if (!name)
    return NULL;
  return !strcmp(name, "WIFI_STA")   ? sta_netif
         : !strcmp(name, "ETHERNET") ? eth_netif
                                     : NULL;
}
bool gw_core_interface_ready(const char *name) {
  esp_netif_t *n = gw_core_uplink_netif(name);
  esp_netif_ip_info_t ip;
  if (ap_route_fault || !n || !esp_netif_is_netif_up(n) ||
      esp_netif_get_ip_info(n, &ip) != ESP_OK || !ip.ip.addr)
    return false;
  return !strcmp(name, "WIFI_STA") ? (sta_connected && sta_has_ip)
                                   : (eth_connected && eth_has_ip);
}
esp_err_t gw_core_ap_start(void) {
  if (!ap_netif)
    return ESP_ERR_INVALID_STATE;
  char ssid[33], password[65];
  esp_err_t e = gw_factory_get_string("ap_ssid", ssid, sizeof(ssid));
  if (e == ESP_OK)
    e = gw_factory_get_string("ap_password", password, sizeof(password));
  if (e != ESP_OK)
    return e;
  if (strlen(password) < 16 || strlen(password) > 63 || !*ssid)
    return ESP_ERR_INVALID_ARG;
  wifi_config_t cfg = {0};
  memcpy(cfg.ap.ssid, ssid, strlen(ssid));
  cfg.ap.ssid_len = strlen(ssid);
  memcpy(cfg.ap.password, password, strlen(password));
  memset(password, 0, sizeof(password));
  cfg.ap.channel = 1;
  cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  cfg.ap.max_connection = 1;
  cfg.ap.pmf_cfg.capable = true;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  e = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (e == ESP_OK)
    e = esp_wifi_set_config(WIFI_IF_AP, &cfg);
  memset(&cfg, 0, sizeof(cfg));
  if (e == ESP_OK) {
    ap_open = true;
    e = ap_subnet(NULL);
  }
  xSemaphoreGive(wifi_lock);
  return e;
}
esp_err_t gw_core_ap_stop(void) {
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
  if (e == ESP_OK) {
    ap_open = false;
    ap_route_fault = false;
  }
  xSemaphoreGive(wifi_lock);
  return e;
}
esp_err_t gw_network_scan(bool active, uint64_t deadline, cJSON **result) {
  if (!result)
    return ESP_ERR_INVALID_ARG;
  *result = NULL;
  uint64_t now = gw_core_mono_ms();
  if (deadline <= now)
    return ESP_ERR_TIMEOUT;
  if (deadline > now + 15000)
    deadline = now + 15000;
  wifi_scan_config_t cfg = {.show_hidden = true,
                            .scan_type = active ? WIFI_SCAN_TYPE_ACTIVE
                                                : WIFI_SCAN_TYPE_PASSIVE};
  if (active) {
    cfg.scan_time.active.min = 30;
    cfg.scan_time.active.max = 120;
  } else {
    cfg.scan_time.passive = 120;
  }
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (scan_running || scan_fault) {
    xSemaphoreGive(wifi_lock);
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(scan_done, 0);
  scan_status = 1;
  scan_running = true;
  esp_err_t e = esp_wifi_scan_start(&cfg, false);
  if (e != ESP_OK)
    scan_running = false;
  xSemaphoreGive(wifi_lock);
  if (e != ESP_OK)
    return e;
  now = gw_core_mono_ms();
  bool completed =
      now < deadline &&
      xSemaphoreTake(scan_done, pdMS_TO_TICKS(deadline - now)) == pdTRUE;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  scan_running = false;
  if (!completed) {
    /* Do not let a delayed SCAN_DONE satisfy the next request. After an
     * aborted scan, further scans require a new driver instance. */
    scan_fault = true;
    (void)esp_wifi_scan_stop();
  }
  uint32_t status = scan_status;
  xSemaphoreGive(wifi_lock);
  if (!completed) {
    (void)esp_wifi_clear_ap_list();
    return ESP_ERR_TIMEOUT;
  }
  if (status) {
    (void)esp_wifi_clear_ap_list();
    return ESP_FAIL;
  }
  uint16_t count = 32;
  wifi_ap_record_t *records = calloc(count, sizeof(*records));
  if (!records)
    return ESP_ERR_NO_MEM;
  e = esp_wifi_scan_get_ap_records(&count, records);
  if (e != ESP_OK) {
    free(records);
    return e;
  }
  cJSON *list = cJSON_CreateArray();
  if (!list) {
    free(records);
    return ESP_ERR_NO_MEM;
  }
  for (unsigned i = 0; i < count; i++) {
    cJSON *ap = cJSON_CreateObject();
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             records[i].bssid[0], records[i].bssid[1], records[i].bssid[2],
             records[i].bssid[3], records[i].bssid[4], records[i].bssid[5]);
    records[i].ssid[sizeof(records[i].ssid) - 1] = 0;
    cJSON_AddStringToObject(ap, "ssid",
                            gw_rules_utf8((const char *)records[i].ssid)
                                ? (const char *)records[i].ssid
                                : "");
    cJSON_AddStringToObject(ap, "bssid", mac);
    cJSON_AddNumberToObject(ap, "channel", records[i].primary);
    cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
    cJSON_AddStringToObject(
        ap, "security",
        records[i].authmode == WIFI_AUTH_WPA3_PSK ? "WPA3_SAE"
        : records[i].authmode == WIFI_AUTH_WPA2_PSK ||
                records[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK
            ? "WPA2_PSK"
            : "UNSUPPORTED");
    cJSON_AddItemToArray(list, ap);
  }
  free(records);
  *result = list;
  return ESP_OK;
}
