#include "gw_signed_time.h"
#include "mbedtls/base64.h"
#include "monocypher-ed25519.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
static const char *s(const cJSON *j, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static bool exact(const cJSON *j, const char *const *keys, size_t n) {
  if (!cJSON_IsObject(j) || cJSON_GetArraySize(j) != (int)n)
    return false;
  for (const cJSON *a = j->child; a; a = a->next) {
    bool found = false;
    for (size_t i = 0; i < n; i++)
      if (a->string && !strcmp(a->string, keys[i]))
        found = true;
    if (!found)
      return false;
    for (const cJSON *b = a->next; b; b = b->next)
      if (!strcmp(a->string, b->string))
        return false;
  }
  return true;
}
static bool decode(const char *s, uint8_t *out, size_t capacity, size_t *n) {
  size_t len = strlen(s);
  if (!len || len % 4 || len > 4 * ((capacity + 2) / 3))
    return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/' ||
          (c == '=' && i >= len - 2)))
      return false;
  }
  if (mbedtls_base64_decode(out, capacity, n, (const unsigned char *)s, len))
    return false;
  unsigned char canonical[1400];
  size_t encoded;
  return !mbedtls_base64_encode(canonical, sizeof(canonical), &encoded, out,
                                *n) &&
         encoded == len && !memcmp(canonical, s, len);
}
esp_err_t gw_signed_time_verify(const cJSON *r, const char *key_id,
                                const uint8_t public_key[32], const char *nonce,
                                const char *boot, uint64_t *issued,
                                uint32_t *uncertainty) {
  static const char *const outer[] = {"keyId", "payloadBase64",
                                      "signatureBase64"};
  static const char *const inner[] = {"schemaVersion", "nonce",
                                      "bootId",        "issuedAtUnixMs",
                                      "uncertaintyMs", "validForMs"};
  if (!key_id || !*key_id || !public_key || !nonce || !boot || !issued ||
      !uncertainty || !exact(r, outer, 3) || strcmp(s(r, "keyId"), key_id))
    return ESP_ERR_INVALID_RESPONSE;
  uint8_t message[1037], sig[64];
  size_t n, sn;
  memcpy(message, "gw32-time-v1", 12);
  if (!decode(s(r, "payloadBase64"), message + 12, 1024, &n) ||
      !decode(s(r, "signatureBase64"), sig, sizeof(sig), &sn) || sn != 64 ||
      crypto_ed25519_check(sig, public_key, message, n + 12))
    return ESP_ERR_INVALID_RESPONSE;
  message[n + 12] = 0;
  if (memchr(message + 12, 0, n))
    return ESP_ERR_INVALID_RESPONSE;
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 12; i < n + 12; i++) {
    char c = message[i];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 1)
        return ESP_ERR_INVALID_RESPONSE;
    } else if (c == '}' || c == ']') {
      if (!depth)
        return ESP_ERR_INVALID_RESPONSE;
      --depth;
    } else if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
      return ESP_ERR_INVALID_RESPONSE;
  }
  if (depth || quoted)
    return ESP_ERR_INVALID_RESPONSE;
  const char *end = NULL;
  cJSON *p = cJSON_ParseWithLengthOpts((char *)message + 12, n + 1, &end, true);
  bool ok = exact(p, inner, 6) && !strcmp(s(p, "schemaVersion"), "1.0") &&
            !strcmp(s(p, "nonce"), nonce) && !strcmp(s(p, "bootId"), boot);
  const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "uncertaintyMs"),
              *v = cJSON_GetObjectItemCaseSensitive(p, "validForMs");
  ok = ok && cJSON_IsNumber(u) && isfinite(u->valuedouble) &&
       u->valuedouble >= 0 && u->valuedouble <= 1000 &&
       floor(u->valuedouble) == u->valuedouble && cJSON_IsNumber(v) &&
       v->valuedouble == 60000;
  const char *digits = s(p, "issuedAtUnixMs");
  uint64_t value = 0;
  if (!*digits || (*digits == '0' && digits[1]))
    ok = false;
  for (const char *q = digits; *q; q++) {
    if (*q < '0' || *q > '9' ||
        value > (UINT64_MAX - (unsigned)(*q - '0')) / 10) {
      ok = false;
      break;
    }
    value = value * 10 + (unsigned)(*q - '0');
  }
  if (ok) {
    *issued = value;
    *uncertainty = (uint32_t)u->valuedouble;
  }
  cJSON_Delete(p);
  return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
