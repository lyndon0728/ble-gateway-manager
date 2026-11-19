#include "gw_internal.h"
#include "gw_rules.h"
#include "mbedtls/sha256.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *gw_json_string(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}
void gw_json_set_string(cJSON *o, const char *key, const char *v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, key);
  cJSON_AddStringToObject(o, key, v ? v : "");
}
void gw_json_set_u64(cJSON *o, const char *key, uint64_t v) {
  char text[24];
  snprintf(text, sizeof(text), "%llu", (unsigned long long)v);
  gw_json_set_string(o, key, text);
}
bool gw_json_u64(const cJSON *v, uint64_t *out) {
  return cJSON_IsString(v) && gw_rules_u64(v->valuestring, out);
}
bool gw_json_allowed(const cJSON *o, const char *const *keys, size_t n) {
  if (!cJSON_IsObject(o))
    return false;
  for (const cJSON *v = o->child; v; v = v->next) {
    bool found = false;
    for (size_t i = 0; i < n; i++)
      if (v->string && !strcmp(v->string, keys[i])) {
        found = true;
        break;
      }
    if (!found)
      return false;
    for (const cJSON *p = v->next; p; p = p->next)
      if (!strcmp(v->string, p->string))
        return false;
  }
  return true;
}
#define utf8 gw_rules_utf8
#define ip4 gw_rules_ipv4
static bool ipv4(const cJSON *o) {
  static const char *keys[] = {"mode", "address", "prefixLength", "gateway",
                               "dns"};
  if (!gw_json_allowed(o, keys, 5))
    return false;
  const char *mode = gw_json_string(o, "mode");
  if (!mode)
    return false;
  if (!strcmp(mode, "DHCP"))
    return cJSON_GetArraySize(o) == 1;
  if (strcmp(mode, "STATIC"))
    return false;
  const cJSON *pre = cJSON_GetObjectItemCaseSensitive(o, "prefixLength"),
              *dns = cJSON_GetObjectItemCaseSensitive(o, "dns");
  if (!cJSON_IsNumber(pre) || !isfinite(pre->valuedouble) ||
      pre->valuedouble != pre->valueint || pre->valueint < 1 ||
      pre->valueint > 30 || !cJSON_IsArray(dns) ||
      cJSON_GetArraySize(dns) < 1 || cJSON_GetArraySize(dns) > 2)
    return false;
  uint32_t address, gateway, mask = UINT32_MAX << (32 - pre->valueint);
  if (!ip4(gw_json_string(o, "address"), &address) ||
      !ip4(gw_json_string(o, "gateway"), &gateway) ||
      (address & mask) != (gateway & mask) || address == gateway ||
      !(address & ~mask) || (address & ~mask) == ~mask || !(gateway & ~mask) ||
      (gateway & ~mask) == ~mask)
    return false;
  for (const cJSON *v = dns->child; v; v = v->next) {
    uint32_t ip;
    if (!cJSON_IsString(v) || !ip4(v->valuestring, &ip))
      return false;
  }
  return true;
}
static esp_err_t invalid(char *error, size_t size, const char *field) {
  if (error && size)
    snprintf(error, size, "%s", field);
  return ESP_ERR_INVALID_ARG;
}
esp_err_t gw_network_validate(const cJSON *n, char *error, size_t size) {
  static const char *top[] = {"uplinkOrder", "ethernet", "wifiSta"};
  static const char *ethkeys[] = {"enabled", "ipv4"};
  static const char *wifikeys[] = {"enabled", "ssid", "security",
                                   "credentialRef", "ipv4"};
  if (!gw_json_allowed(n, top, 3) || cJSON_GetArraySize(n) != 3)
    return invalid(error, size, "network");
  const cJSON *eth = cJSON_GetObjectItemCaseSensitive(n, "ethernet"),
              *wifi = cJSON_GetObjectItemCaseSensitive(n, "wifiSta"),
              *order = cJSON_GetObjectItemCaseSensitive(n, "uplinkOrder");
  if (!gw_json_allowed(eth, ethkeys, 2) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(eth, "enabled")) ||
      !ipv4(cJSON_GetObjectItemCaseSensitive(eth, "ipv4")))
    return invalid(error, size, "ethernet");
  if (!gw_json_allowed(wifi, wifikeys, 5) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(wifi, "enabled")) ||
      !ipv4(cJSON_GetObjectItemCaseSensitive(wifi, "ipv4")))
    return invalid(error, size, "wifiSta");
  bool e = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eth, "enabled")),
       w = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wifi, "enabled"));
  if (e && !gw_core_uplink_netif("ETHERNET"))
    return ESP_ERR_NOT_SUPPORTED;
  if (w) {
    const char *ssid = gw_json_string(wifi, "ssid"),
               *sec = gw_json_string(wifi, "security"),
               *ref = gw_json_string(wifi, "credentialRef");
    if (!ssid || !strlen(ssid) || strlen(ssid) > 32 || !utf8(ssid))
      return invalid(error, size, "wifiSta.ssid");
    if (!sec || (strcmp(sec, "WPA2_PSK") && strcmp(sec, "WPA3_SAE")))
      return invalid(error, size, "wifiSta.security");
    if (!ref || !*ref || strlen(ref) > 128)
      return invalid(error, size, "wifiSta.credentialRef");
    char password[65] = {0};
    esp_err_t err = gw_credential_password(ref, ssid, sec, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK)
      return ESP_ERR_NOT_FOUND;
  }
  int count = cJSON_GetArraySize(order);
  if (!cJSON_IsArray(order) || count < 1 || count > 2)
    return invalid(error, size, "uplinkOrder");
  const char *prev = NULL;
  for (const cJSON *v = order->child; v; v = v->next) {
    if (!cJSON_IsString(v) ||
        (!strcmp(v->valuestring, "ETHERNET")   ? !e
         : !strcmp(v->valuestring, "WIFI_STA") ? !w
                                               : true) ||
        (prev && !strcmp(prev, v->valuestring)))
      return invalid(error, size, "uplinkOrder");
    prev = v->valuestring;
  }
  return ESP_OK;
}
static int key_compare(const void *a, const void *b) {
  return strcmp((*(const cJSON *const *)a)->string,
                (*(const cJSON *const *)b)->string);
}
static cJSON *canonical(const cJSON *v, unsigned depth) {
  if (depth > 16)
    return NULL;
  if (cJSON_IsObject(v)) {
    int n = cJSON_GetArraySize(v);
    if (n > 64)
      return NULL;
    const cJSON *items[64];
    int i = 0;
    for (const cJSON *x = v->child; x; x = x->next)
      items[i++] = x;
    qsort(items, n, sizeof(items[0]), key_compare);
    for(i=1;i<n;i++)if(!strcmp(items[i-1]->string,items[i]->string))return NULL;
    cJSON *o = cJSON_CreateObject();
    if (!o)
      return NULL;
    for (i = 0; i < n; i++) {
      cJSON *x = canonical(items[i], depth + 1);
      if (!x || !cJSON_AddItemToObject(o, items[i]->string, x)) {
        cJSON_Delete(x);
        cJSON_Delete(o);
        return NULL;
      }
    }
    return o;
  }
  if (cJSON_IsArray(v)) {
    cJSON *a = cJSON_CreateArray();
    if (!a)
      return NULL;
    for (const cJSON *x = v->child; x; x = x->next) {
      cJSON *item = canonical(x, depth + 1);
      if (!item || !cJSON_AddItemToArray(a, item)) {
        cJSON_Delete(item);
        cJSON_Delete(a);
        return NULL;
      }
    }
    return a;
  }
  if(cJSON_IsNumber(v)&&isfinite(v->valuedouble)&&
     fabs(v->valuedouble)<=9007199254740991.0&&floor(v->valuedouble)==v->valuedouble) {
    char integer[32];snprintf(integer,sizeof(integer),"%.0f",v->valuedouble==0?0:v->valuedouble);
    return cJSON_CreateRaw(integer);
  }
  return cJSON_Duplicate(v, true);
}
esp_err_t gw_core_network_snapshot_id(const cJSON *network, char hex[65]) {
  /* Validated network contains only known ASCII keys, bounded UTF-8 strings,
   * booleans and integral prefixLength; sorted cJSON encoding matches JCS. */
  char *text = gw_json_canonical(network);
  if (!text)
    return ESP_ERR_NO_MEM;
  uint8_t hash[32];
  int rc = mbedtls_sha256((const unsigned char *)text, strlen(text), hash, 0);
  free(text);
  if (rc)
    return ESP_FAIL;
  static const char chars[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; i++) {
    hex[2 * i] = chars[hash[i] >> 4];
    hex[2 * i + 1] = chars[hash[i] & 15];
  }
  hex[64] = 0;
  return ESP_OK;
}
char *gw_json_canonical(const cJSON *value) {
  cJSON *ordered = canonical(value, 0);
  if (!ordered)
    return NULL;
  char *text = cJSON_PrintUnformatted(ordered);
  cJSON_Delete(ordered);
  return text;
}
