#include "../gw_radio_evidence.h"
#include "gw_nrf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n,
                          const unsigned char *in, size_t size) {
  static const char b[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (size % 4)
    return -1;
  size_t j = 0;
  for (size_t i = 0; i < size; i += 4) {
    uint32_t v = 0;
    for (unsigned k = 0; k < 4; k++) {
      const char *p = strchr(b, in[i + k]);
      if (!p && in[i + k] != '=')
        return -1;
      v = (v << 6) | (p ? (unsigned)(p - b) : 0);
    }
    if (j >= cap)
      return -1;
    out[j++] = (uint8_t)(v >> 16);
    if (in[i + 2] != '=') {
      if (j >= cap)
        return -1;
      out[j++] = (uint8_t)(v >> 8);
    }
    if (in[i + 3] != '=') {
      if (j >= cap)
        return -1;
      out[j++] = (uint8_t)v;
    }
  }
  *n = j;
  return 0;
}
static const char *base =
    "{\"scan\":{\"enabled\":true,\"active\":true,\"intervalMs\":100,"
    "\"windowMs\":50},\"filters\":{\"minRssi\":null,\"serviceUuids\":[],"
    "\"manufacturerIds\":[]},\"beaconRules\":[]}";
static const char *rule =
    "{\"action\":\"ALLOW\",\"reportKinds\":[\"LEGACY_ADV\"],\"fieldMask\":31,"
    "\"name\":{\"mode\":\"PREFIX\",\"source\":\"COMPLETE\",\"valueBase64\":"
    "\"QmNuLQ==\",\"maskBase64\":\"/////"
    "w==\"},\"ibeaconUuid\":{\"value\":\"fda50693-a4e2-4fb1-afcf-"
    "c6eb07647825\",\"maskHex\":\"ffffffffffffffffffffffffffffffff\"},\"rssi\":"
    "{\"mode\":\"RANGE\",\"minDbm\":-75,\"maxDbm\":-35},\"major\":{\"value\":"
    "4608,\"mask\":65280},\"minor\":{\"value\":3,\"mask\":15}}";
static cJSON *configuration(void) {
  cJSON *o = cJSON_Parse(base),
        *v = cJSON_AddObjectToObject(o, "beaconFilterV2");
  cJSON_AddNumberToObject(v, "version", 2);
  cJSON_AddStringToObject(v, "scope", "ADV_SCAN_RSP_PAIR");
  cJSON_AddNumberToObject(v, "pairWindowMs", 500);
  cJSON_AddItemToArray(cJSON_AddArrayToObject(v, "rules"), cJSON_Parse(rule));
  return o;
}
static cJSON *first_rule(cJSON *o) {
  return cJSON_GetArrayItem(
      cJSON_GetObjectItem(cJSON_GetObjectItem(o, "beaconFilterV2"), "rules"),
      0);
}
static void reject(cJSON *o) {
  uint8_t out[3070];
  size_t n = 123;
  assert(gw_nrf_compile_scan(o, 1, out, sizeof(out), &n) != ESP_OK);
  assert(!n);
  cJSON_Delete(o);
}
static void advertising_evidence_test(void) {
  uint8_t desired[14] = {0}, actual[35] = {0}, peripheral[16] = {0};
  gw_put_u64(desired, 9);
  gw_put_u64(actual, 9);
  desired[8] = actual[8] = 1;
  desired[9] = actual[10] = 2;
  gw_put_u16(desired + 10, 160);
  gw_put_u16(actual + 11, 160);
  actual[9] = 0;
  assert(gw_advertising_evidence(desired, 14, actual, 35) ==
         ESP_ERR_INVALID_STATE);
  actual[9] = 1;
  assert(gw_advertising_evidence(desired, 14, actual, 35) == ESP_OK);
  actual[9] = 2;
  assert(gw_advertising_evidence(desired, 14, actual, 35) ==
         ESP_ERR_INVALID_STATE);
  actual[9] = 3;
  assert(gw_advertising_evidence(desired, 14, actual, 35) ==
         ESP_ERR_NOT_FINISHED);
  assert(!gw_peripheral_live(peripheral, 16));
  gw_put_u32(peripheral, 4);
  peripheral[4] = 1;
  assert(gw_peripheral_live(peripheral, 16));
  peripheral[4] = 0;
  assert(!gw_peripheral_live(peripheral, 16));
  desired[8] = actual[8] = 0;
  actual[9] = 0;
  assert(gw_advertising_evidence(desired, 14, actual, 35) == ESP_OK);
  actual[15] = 1;
  assert(gw_advertising_evidence(desired, 14, actual, 35) ==
         ESP_ERR_INVALID_RESPONSE);
  actual[15] = 0;
  actual[0] = 8;
  assert(gw_advertising_evidence(desired, 14, actual, 35) ==
         ESP_ERR_INVALID_RESPONSE);
  assert(gw_advertising_evidence(desired, 14, actual, 34) ==
         ESP_ERR_INVALID_RESPONSE);
}
int main(void) {
  advertising_evidence_test();
  uint8_t out[3070], expected[87];
  size_t n;
  const char *hex =
      "020113001000010000000000000003a00050008000000001f401013a0001011f00010004"
      "42636e2dfffffffffda50693a4e24fb1afcfc6eb07647825ffffffffffffffffffffffff"
      "ffffffff00b5dd001200ff03000f00";
  for (unsigned i = 0; i < 87; i++) {
    char t[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    expected[i] = (uint8_t)strtoul(t, NULL, 16);
  }
  cJSON *o = configuration();
  assert(gw_nrf_compile_scan(o, 1, out, sizeof(out), &n) == ESP_OK);
  assert(n == 87 && !memcmp(out, expected, 87));
  struct gw_filter_v2_config parsed;
  assert(gw_filter_v2_parse(out, n, &parsed));
  assert(parsed.scope == 1 && parsed.pair_window_ms == 500);
  assert(gw_nrf_compile_scan(o, 1, out, 86, &n) != ESP_OK);
  cJSON_Delete(o);
  o = configuration();
  cJSON_AddNumberToObject(first_rule(o), "unknown", 1);
  reject(o);
  o = configuration();
  cJSON_AddNumberToObject(first_rule(o), "fieldMask", 31);
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(first_rule(o), "fieldMask", cJSON_CreateNumber(15));
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(cJSON_GetObjectItem(first_rule(o), "rssi"),
                            "minDbm", cJSON_CreateNumber(-128));
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(cJSON_GetObjectItem(first_rule(o), "major"), "mask",
                            cJSON_CreateNumber(0));
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(cJSON_GetObjectItem(o, "scan"), "active",
                            cJSON_CreateFalse());
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(cJSON_GetObjectItem(o, "filters"), "serviceUuids",
                            cJSON_CreateObject());
  reject(o);
  o = configuration();
  cJSON_ReplaceItemInObject(cJSON_GetObjectItem(first_rule(o), "name"),
                            "maskBase64", cJSON_CreateString("AAAAAA=="));
  reject(o);
  o = configuration();
  cJSON_AddItemToArray(
      cJSON_GetObjectItem(o, "beaconRules"),
      cJSON_Parse("{\"action\":\"ALLOW\",\"reportKinds\":[\"LEGACY_ADV\"],"
                  "\"adType\":255,\"offset\":0,\"valueBase64\":\"AA==\","
                  "\"maskBase64\":\"/w==\"}"));
  reject(o);
  o = cJSON_Parse(base);
  assert(gw_nrf_compile_scan(o, 7, out, sizeof(out), &n) == ESP_OK);
  assert(n == 27 && out[1] == 0 && gw_filter_v2_parse(out, n, &parsed));
  cJSON_Delete(o);
  o = configuration();
  cJSON *v = cJSON_GetObjectItem(o, "beaconFilterV2");
  cJSON_DeleteItemFromObject(v, "pairWindowMs");
  cJSON_ReplaceItemInObject(v, "scope", cJSON_CreateString("REPORT"));
  cJSON *rss = cJSON_Parse("{\"mode\":\"BITS\",\"valueDbm\":-60,\"mask\":255}");
  cJSON_ReplaceItemInObject(first_rule(o), "rssi", rss);
  assert(gw_nrf_compile_scan(o, 9, out, sizeof(out), &n) == ESP_OK &&
         gw_filter_v2_parse(out, n, &parsed));
  assert(parsed.rules[0].rssi_mode == 1 &&
         parsed.rules[0].rssi_arg0 == (uint8_t)-60);
  cJSON_Delete(o);
  puts("PASS: scan compiler golden V2, engine switch, masks, malformed "
       "contracts, size limit");
  return 0;
}
