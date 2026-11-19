#include "gw_nrf.h"
#include "mbedtls/base64.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double number(const cJSON *o, const char *key, double fallback) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}
static bool uuid(const char *s, uint8_t out[16]) {
  char compact[33];
  size_t n = 0;
  if (!s)
    return false;
  if (strlen(s) == 4) {
    char expanded[37];
    snprintf(expanded, sizeof(expanded), "0000%s-0000-1000-8000-00805f9b34fb",
             s);
    return uuid(expanded, out);
  }
  if (strlen(s) == 8) {
    char expanded[37];
    snprintf(expanded, sizeof(expanded), "%s-0000-1000-8000-00805f9b34fb", s);
    return uuid(expanded, out);
  }
  if (strlen(s) != 36)
    return false;
  for (size_t i = 0; s[i]; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return false;
      continue;
    }
    if (!isxdigit((unsigned char)s[i]) || n >= 32)
      return false;
    compact[n++] = s[i];
  }
  if (n != 32)
    return false;
  compact[32] = 0;
  for (size_t i = 0; i < 16; ++i) {
    char a[3] = {compact[2 * i], compact[2 * i + 1], 0};
    char *end;
    unsigned long v = strtoul(a, &end, 16);
    if (*end)
      return false;
    out[i] = v;
  }
  return true;
}
static int uuid_cmp(const void *a, const void *b) { return memcmp(a, b, 16); }
static int company_cmp(const void *a, const void *b) {
  uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
  return (x > y) - (x < y);
}
static bool b64(const cJSON *o, const char *key, uint8_t *out, size_t cap,
                size_t *n) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsString(v))
    return false;
  return mbedtls_base64_decode(out, cap, n,
                               (const unsigned char *)v->valuestring,
                               strlen(v->valuestring)) == 0;
}

static esp_err_t compile_legacy(const cJSON *complete, uint64_t revision,
                                uint8_t *out, size_t cap, size_t *size) {
  struct gw_profile *p = calloc(1, sizeof(*p));
  uint8_t *data = malloc(GW_PROFILE_MAX);
  if (!p || !data) {
    free(p);
    free(data);
    return ESP_ERR_NO_MEM;
  }
  gw_profile_default(p);
  p->revision = revision;
  const cJSON *scan = cJSON_GetObjectItemCaseSensitive(complete, "scan"),
              *filters = cJSON_GetObjectItemCaseSensitive(complete, "filters");
  esp_err_t e = ESP_ERR_INVALID_ARG;
  if (!cJSON_IsObject(scan) || !cJSON_IsObject(filters) || !revision ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(scan, "enabled")) ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(scan, "active")))
    goto done;
  const cJSON *check = cJSON_GetObjectItemCaseSensitive(scan, "intervalMs");
  if (check && !cJSON_IsNumber(check))
    goto done;
  check = cJSON_GetObjectItemCaseSensitive(scan, "windowMs");
  if (check && !cJSON_IsNumber(check))
    goto done;
  check = cJSON_GetObjectItemCaseSensitive(filters, "minRssi");
  if (check && !cJSON_IsNull(check) && !cJSON_IsNumber(check))
    goto done;
  double interval = number(scan, "intervalMs", 100),
         window = number(scan, "windowMs", 50),
         rssi = number(filters, "minRssi", -128);
  if (!isfinite(interval) || !isfinite(window) || !isfinite(rssi) ||
      interval < 10 || interval > 10240 || window < 10 || window > interval ||
      floor(interval) != interval || floor(window) != window || rssi < -128 ||
      rssi > 20 || floor(rssi) != rssi)
    goto done;
  p->interval_ticks = ((uint32_t)interval * 8 + 2) / 5;
  p->window_ticks = ((uint32_t)window * 8 + 2) / 5;
  p->min_rssi = (int8_t)rssi;
  p->scan_flags =
      (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(scan, "enabled")) ? 1
                                                                       : 0) |
      (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(scan, "active")) ? 2 : 0);
  const cJSON *v,
      *list = cJSON_GetObjectItemCaseSensitive(filters, "serviceUuids");
  if (list && !cJSON_IsArray(list))
    goto done;
  cJSON_ArrayForEach(v, list) {
    if (p->uuid_count == 128 || !cJSON_IsString(v) ||
        !uuid(v->valuestring, p->uuids[p->uuid_count]))
      goto done;
    ++p->uuid_count;
  }
  qsort(p->uuids, p->uuid_count, 16, uuid_cmp);
  unsigned used = 0;
  for (unsigned i = 0; i < p->uuid_count; i++)
    if (!used || memcmp(p->uuids[i], p->uuids[used - 1], 16))
      memcpy(p->uuids[used++], p->uuids[i], 16);
  p->uuid_count = used;
  list = cJSON_GetObjectItemCaseSensitive(filters, "manufacturerIds");
  if (list && !cJSON_IsArray(list))
    goto done;
  cJSON_ArrayForEach(v, list) {
    if (p->company_count == 128 || !cJSON_IsNumber(v) || v->valuedouble < 0 ||
        v->valuedouble > 65535 || floor(v->valuedouble) != v->valuedouble)
      goto done;
    p->companies[p->company_count++] = (uint16_t)v->valuedouble;
  }
  qsort(p->companies, p->company_count, 2, company_cmp);
  used = 0;
  for (unsigned i = 0; i < p->company_count; i++)
    if (!used || p->companies[i] != p->companies[used - 1])
      p->companies[used++] = p->companies[i];
  p->company_count = used;
  list = cJSON_GetObjectItemCaseSensitive(complete, "beaconRules");
  if (list && !cJSON_IsArray(list))
    goto done;
  cJSON_ArrayForEach(v, list) {
    if (p->rule_count == 8)
      goto done;
    struct gw_beacon_rule *rule = &p->rules[p->rule_count];
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(v, "action");
    if (!cJSON_IsString(action) || (strcmp(action->valuestring, "ALLOW") &&
                                    strcmp(action->valuestring, "DENY")))
      goto done;
    rule->action = !strcmp(action->valuestring, "ALLOW");
    rule->kind_mask = 0;
    const cJSON *kind,
        *kinds = cJSON_GetObjectItemCaseSensitive(v, "reportKinds");
    if (!cJSON_IsArray(kinds))
      goto done;
    cJSON_ArrayForEach(kind, kinds) {
      if (!cJSON_IsString(kind))
        goto done;
      const char *names[] = {"LEGACY_ADV", "LEGACY_SCAN_RSP", "EXT_ADV",
                             "EXT_SCAN_RSP"};
      bool known = false;
      for (unsigned k = 0; k < 4; k++)
        if (!strcmp(kind->valuestring, names[k])) {
          rule->kind_mask |= 1U << k;
          known = true;
        }
      if (!known)
        goto done;
    }
    double type = number(v, "adType", 0), offset = number(v, "offset", -1);
    if (type < 1 || type > 255 || floor(type) != type || offset < 0 ||
        offset > 253 || floor(offset) != offset)
      goto done;
    rule->ad_type = (uint8_t)type;
    rule->offset = (uint16_t)offset;
    size_t n = 0, m = 0;
    if (!b64(v, "valueBase64", rule->value, 24, &n) ||
        !b64(v, "maskBase64", rule->mask, 24, &m) || n != m || !n)
      goto done;
    if (!rule->kind_mask || offset + n > 254)
      goto done;
    bool any = false;
    for (size_t k = 0; k < n; k++)
      any |= rule->mask[k] != 0;
    if (!any)
      goto done;
    rule->length = n;
    p->rule_count++;
  }
  size_t n = gw_profile_encode(p, true, data, GW_PROFILE_MAX);
  if (!n)
    goto done;
  if (n > cap)
    goto done;
  memcpy(out, data, n);
  *size = n;
  e = ESP_OK;
done:
  free(p);
  free(data);
  return e;
}

static bool keys(const cJSON *o, const char *const *names, size_t count) {
  if (!cJSON_IsObject(o))
    return false;
  const cJSON *v;
  unsigned seen = 0;
  cJSON_ArrayForEach(v, o) {
    bool found = false;
    for (size_t i = 0; i < count; i++)
      if (v->string && !strcmp(v->string, names[i])) {
        if (seen & (1u << i))
          return false;
        seen |= 1u << i;
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  return true;
}
static bool exact_keys(const cJSON *o, const char *const *names, size_t count) {
  return keys(o, names, count) && cJSON_GetArraySize(o) == (int)count;
}
static bool integer_value(const cJSON *o, const char *key, int low, int high,
                          int *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
      v->valuedouble > high || floor(v->valuedouble) != v->valuedouble)
    return false;
  *out = (int)v->valuedouble;
  return true;
}
static int choice(const cJSON *o, const char *key, const char *const *values,
                  size_t count) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (cJSON_IsString(v))
    for (size_t i = 0; i < count; i++)
      if (!strcmp(v->valuestring, values[i]))
        return (int)i;
  return -1;
}
static bool nonzero(const uint8_t *p, size_t n) {
  uint8_t bits = 0;
  while (n--)
    bits |= *p++;
  return bits != 0;
}
static bool uuid_predicate(const cJSON *o, uint8_t *out) {
  const char *const names[] = {"value", "maskHex"};
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(o, "value"),
              *mask = cJSON_GetObjectItemCaseSensitive(o, "maskHex");
  if (!exact_keys(o, names, 2) || !cJSON_IsString(value) ||
      strlen(value->valuestring) != 36 || !uuid(value->valuestring, out) ||
      !cJSON_IsString(mask) || strlen(mask->valuestring) != 32)
    return false;
  for (unsigned i = 0; i < 32; i++) {
    char c = mask->valuestring[i];
    if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
      return false;
    unsigned x = c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
    if (!(i & 1))
      out[16 + i / 2] = (uint8_t)(x << 4);
    else
      out[16 + i / 2] |= x;
  }
  for (const char *c = value->valuestring; *c; c++)
    if (*c >= 'A' && *c <= 'F')
      return false;
  return nonzero(out + 16, 16);
}

esp_err_t gw_nrf_compile_scan(const cJSON *complete, uint64_t revision,
                              uint8_t *out, size_t capacity, size_t *size) {
  if (!out || !size || capacity < 27 || !cJSON_IsObject(complete) ||
      !revision || revision > UINT64_C(9007199254740991))
    return ESP_ERR_INVALID_ARG;
  *size = 0;
  if (capacity > 3070)
    capacity = 3070;
  size_t legacy_size = 0;
  esp_err_t e =
      compile_legacy(complete, revision, out + 4, capacity - 8, &legacy_size);
  if (e != ESP_OK)
    return e;
  const cJSON *v2 =
      cJSON_GetObjectItemCaseSensitive(complete, "beaconFilterV2");
  bool active = v2 && !cJSON_IsNull(v2);
  out[0] = 2;
  out[1] = active;
  gw_put_u16(out + 2, legacy_size);
  size_t pos = 4 + legacy_size;
  memset(out + pos, 0, 4);
  pos += 4;
  if (!active) {
    *size = pos;
    return ESP_OK;
  }
  const char *const top[] = {"version", "scope", "pairWindowMs", "rules"};
  const char *const scopes[] = {"REPORT", "ADV_SCAN_RSP_PAIR"};
  int version, scope = choice(v2, "scope", scopes, 2), window = 0;
  if (!keys(v2, top, 4) || !integer_value(v2, "version", 2, 2, &version) ||
      scope < 0 || cJSON_GetArraySize(v2) != (scope ? 4 : 3) ||
      out[4 + 2 + gw_get_u16(out + 4)] != 0)
    return ESP_ERR_INVALID_ARG;
  if (scope) {
    if (!integer_value(v2, "pairWindowMs", 500, 500, &window) ||
        !(out[4 + 2 + 8] & 2))
      return ESP_ERR_INVALID_ARG;
  } else if (cJSON_GetObjectItemCaseSensitive(v2, "pairWindowMs"))
    return ESP_ERR_INVALID_ARG;
  const cJSON *rules = cJSON_GetObjectItemCaseSensitive(v2, "rules"), *r;
  if (!cJSON_IsArray(rules) || cJSON_GetArraySize(rules) < 1 ||
      cJSON_GetArraySize(rules) > 8)
    return ESP_ERR_INVALID_ARG;
  out[pos - 4] = scope;
  gw_put_u16(out + pos - 3, window);
  out[pos - 1] = cJSON_GetArraySize(rules);
  cJSON_ArrayForEach(r, rules) {
    const char *const rn[] = {"action", "reportKinds", "fieldMask",
                              "name",   "ibeaconUuid", "rssi",
                              "major",  "minor",       "serviceUuid"};
    const char *const actions[] = {"DENY", "ALLOW"};
    const char *const kinds[] = {"LEGACY_ADV", "LEGACY_SCAN_RSP", "EXT_ADV",
                                 "EXT_SCAN_RSP"};
    const char *const fields[] = {"name",  "ibeaconUuid", "rssi",
                                  "major", "minor",       "serviceUuid"};
    int field_mask, action = choice(r, "action", actions, 2);
    if (!keys(r, rn, 9) || action < 0 ||
        !integer_value(r, "fieldMask", 1, 63, &field_mask))
      return ESP_ERR_INVALID_ARG;
    const cJSON *ks = cJSON_GetObjectItemCaseSensitive(r, "reportKinds"), *k;
    unsigned kind_mask = 0;
    if (!cJSON_IsArray(ks) || cJSON_GetArraySize(ks) < 1 ||
        cJSON_GetArraySize(ks) > 4)
      return ESP_ERR_INVALID_ARG;
    cJSON_ArrayForEach(k, ks) {
      unsigned bit = 0;
      for (unsigned i = 0; i < 4; i++)
        if (cJSON_IsString(k) && !strcmp(k->valuestring, kinds[i]))
          bit = 1u << i;
      if (!bit || (kind_mask & bit))
        return ESP_ERR_INVALID_ARG;
      kind_mask |= bit;
    }
    if (scope && kind_mask != 1)
      return ESP_ERR_INVALID_ARG;
    uint8_t rule[140];
    size_t used = 4;
    rule[0] = action;
    rule[1] = kind_mask;
    gw_put_u16(rule + 2, field_mask);
    for (unsigned bit = 0; bit < 6; bit++) {
      const cJSON *field = cJSON_GetObjectItemCaseSensitive(r, fields[bit]);
      if (!(field_mask & (1u << bit))) {
        if (field)
          return ESP_ERR_INVALID_ARG;
        continue;
      }
      if (!cJSON_IsObject(field))
        return ESP_ERR_INVALID_ARG;
      if (bit == 0) {
        const char *const names[] = {"mode", "source", "valueBase64",
                                     "maskBase64"};
        const char *const modes[] = {"EXACT", "PREFIX"}, *const sources[] = {
                                                             "COMPLETE",
                                                             "SHORTENED",
                                                             "ANY"};
        int mode = choice(field, "mode", modes, 2),
            source = choice(field, "source", sources, 3);
        uint8_t value[29], mask[29];
        size_t n = 0, m = 0;
        if (!exact_keys(field, names, 4) || mode < 0 || source < 0 ||
            !b64(field, "valueBase64", value, 29, &n) ||
            !b64(field, "maskBase64", mask, 29, &m) || !n || n != m ||
            !nonzero(mask, m))
          return ESP_ERR_INVALID_ARG;
        rule[used++] = mode;
        rule[used++] = source;
        rule[used++] = n;
        memcpy(rule + used, value, n);
        used += n;
        memcpy(rule + used, mask, n);
        used += n;
      } else if (bit == 1 || bit == 5) {
        if (!uuid_predicate(field, rule + used))
          return ESP_ERR_INVALID_ARG;
        used += 32;
      } else if (bit == 2) {
        const char *const modes[] = {"RANGE", "BITS"};
        const char *const range[] = {"mode", "minDbm", "maxDbm"},
                          *const bits[] = {"mode", "valueDbm", "mask"};
        int mode = choice(field, "mode", modes, 2), a, b;
        if (mode < 0 || !exact_keys(field, mode ? bits : range, 3) ||
            !integer_value(field, mode ? "valueDbm" : "minDbm", -127, 20, &a) ||
            !integer_value(field, mode ? "mask" : "maxDbm", mode ? 1 : -127,
                           mode ? 255 : 20, &b) ||
            (!mode && a > b))
          return ESP_ERR_INVALID_ARG;
        rule[used++] = mode;
        rule[used++] = (uint8_t)a;
        rule[used++] = (uint8_t)b;
      } else {
        const char *const names[] = {"value", "mask"};
        int a, b;
        if (!exact_keys(field, names, 2) ||
            !integer_value(field, "value", 0, 65535, &a) ||
            !integer_value(field, "mask", 1, 65535, &b))
          return ESP_ERR_INVALID_ARG;
        gw_put_u16(rule + used, a);
        gw_put_u16(rule + used + 2, b);
        used += 4;
      }
    }
    if (used + 2 > capacity - pos)
      return ESP_ERR_INVALID_SIZE;
    gw_put_u16(out + pos, used);
    pos += 2;
    memcpy(out + pos, rule, used);
    pos += used;
  }
  *size = pos;
  return ESP_OK;
}
