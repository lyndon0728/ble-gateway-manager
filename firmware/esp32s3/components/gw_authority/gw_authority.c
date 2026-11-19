#include "gw_authority.h"
#include "gw_core.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "monocypher-ed25519.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static cJSON *authorized, *board;
static char manifest_digest[65];
static esp_err_t factory_text(const char *key, char *out, size_t capacity) {
  esp_err_t e = gw_factory_get_string(key, out, capacity);
  if (e == ESP_OK)
    return e;
  size_t n = capacity - 1;
  e = gw_factory_get_blob(key, out, &n);
  if (e != ESP_OK || n >= capacity || memchr(out, 0, n))
    return ESP_ERR_INVALID_RESPONSE;
  out[n] = 0;
  return ESP_OK;
}
static const char *s(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static bool unique(const cJSON *j, unsigned depth) {
  if (depth > 12)
    return false;
  for (const cJSON *v = j ? j->child : NULL; v; v = v->next) {
    if (cJSON_IsObject(j))
      for (const cJSON *n = v->next; n; n = n->next)
        if (!strcmp(v->string, n->string))
          return false;
    if (!unique(v, depth + 1))
      return false;
  }
  return true;
}
static bool contains(const cJSON *array, const char *v) {
  for (const cJSON *x = array ? array->child : NULL; x; x = x->next)
    if (cJSON_IsString(x) && !strcmp(x->valuestring, v))
      return true;
  return false;
}
static bool capability_name(const char *name) {
  if (!name || name[0]<'A' || name[0]>'Z' || strlen(name)>96)
    return false;
  for (const char *p=name+1;*p;p++)
    if (!((*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'))
      return false;
  return true;
}
esp_err_t gw_authority_verify(const char *manifest, const char *artifact,
                              const uint8_t signature[64],
                              const uint8_t public_key[32], const char *key_id,
                              const char *gateway, const char *sn,
                              const char *model, const char *hardware,
                              cJSON **caps, char digest[65]) {
  if (!caps || !digest)
    return ESP_ERR_INVALID_ARG;
  *caps = NULL;
  digest[0] = 0;
  if (!manifest || !artifact || strlen(manifest) > 65536 ||
      strlen(artifact) > 8192 || !signature || !public_key || !key_id ||
      !*key_id || !gateway || !*gateway || !sn || !*sn || !model || !*model ||
      !hardware || !*hardware || strstr(manifest, "\\u0000") ||
      strstr(artifact, "\\u0000") ||
      crypto_ed25519_check(signature, public_key, (const uint8_t *)artifact,
                           strlen(artifact)))
    return ESP_ERR_INVALID_RESPONSE;
  const char *end = NULL;
  cJSON *a =
      cJSON_ParseWithLengthOpts(artifact, strlen(artifact) + 1, &end, true);
  cJSON *m =
      cJSON_ParseWithLengthOpts(manifest, strlen(manifest) + 1, &end, true);
  bool valid = cJSON_IsObject(a) && cJSON_IsObject(m) && unique(a, 0) &&
               unique(m, 0) && !strcmp(s(a, "kind"), "DEVICE_MANIFEST") &&
               !strcmp(s(a, "keyId"), key_id) &&
               !strcmp(s(m, "schemaVersion"), "1.0");
  uint8_t sha[32] = {0};
  char hex[65];
  static const char h[] = "0123456789abcdef";
  if (mbedtls_sha256((const uint8_t *)manifest, strlen(manifest), sha, 0))
    valid = false;
  for (unsigned i = 0; i < 32; i++) {
    hex[2 * i] = h[sha[i] >> 4];
    hex[2 * i + 1] = h[sha[i] & 15];
  }
  hex[64] = 0;
  valid = valid && !strcmp(s(a, "sha256"), hex);
  const cJSON *devices = cJSON_GetObjectItemCaseSensitive(m, "devices"),
              *entry = NULL;
  if (!cJSON_IsArray(devices))
    valid = false;
  for (const cJSON *d = devices ? devices->child : NULL; d; d = d->next)
    if (!strcmp(s(d, "gatewayId"), gateway)) {
      if (entry)
        valid = false;
      entry = d;
    }
  valid = valid && entry && !strcmp(s(entry, "sn"), sn) &&
          !strcmp(s(entry, "model"), model) &&
          !strcmp(s(entry, "hardwareVersion"), hardware);
  const cJSON *list = cJSON_GetObjectItemCaseSensitive(entry, "capabilities");
  if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > 128)
    valid = false;
  for (const cJSON *v = list ? list->child : NULL; v; v = v->next) {
    if (!cJSON_IsString(v) || !capability_name(v->valuestring)) {
      valid = false;
      break;
    }
    for (const cJSON *n = v->next; n; n = n->next)
      if (cJSON_IsString(n) && !strcmp(n->valuestring, v->valuestring))
        valid = false;
  }
  if (valid) {
    *caps = cJSON_Duplicate(list, true);
    if (!*caps)
      valid = false;
    else
      memcpy(digest, hex, 65);
  }
  cJSON_Delete(a);
  cJSON_Delete(m);
  return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
esp_err_t gw_authority_init(void) {
  cJSON_Delete(authorized);
  authorized = NULL;
  cJSON_Delete(board);
  board = NULL;
  manifest_digest[0] = 0;
  char *manifest = calloc(1, 65537), *artifact = calloc(1, 8193),
       *board_json = calloc(1, 8192);
  if (!manifest || !artifact || !board_json) {
    free(manifest);
    free(artifact);
    free(board_json);
    return ESP_ERR_NO_MEM;
  }
  char key_id[65], gateway[129], sn[129], model[129], hardware[65];
  uint8_t signature[64], key[32];
  size_t signature_n = 64, key_n = 32;
  esp_err_t e = ESP_ERR_INVALID_STATE;
  if (factory_text("dev_manifest", manifest, 65537) == ESP_OK &&
      factory_text("dev_artifact", artifact, 8193) == ESP_OK &&
      gw_factory_get_string("manifest_keyid", key_id, sizeof(key_id)) ==
          ESP_OK &&
      gw_factory_get_blob("dev_signature", signature, &signature_n) == ESP_OK &&
      signature_n == 64 &&
      gw_factory_get_blob("manifest_pub", key, &key_n) == ESP_OK &&
      key_n == 32 &&
      gw_factory_get_string("gateway", gateway, sizeof(gateway)) == ESP_OK &&
      gw_factory_get_string("sn", sn, sizeof(sn)) == ESP_OK &&
      gw_factory_get_string("model", model, sizeof(model)) == ESP_OK &&
      gw_factory_get_string("hardware", hardware, sizeof(hardware)) == ESP_OK)
    e = gw_authority_verify(manifest, artifact, signature, key, key_id, gateway,
                            sn, model, hardware, &authorized, manifest_digest);
  if (e == ESP_OK &&
      gw_factory_get_string("board_caps", board_json, 8192) == ESP_OK) {
    const char *tail = NULL;
    board = cJSON_ParseWithLengthOpts(board_json, strlen(board_json) + 1, &tail,
                                      true);
    if (!cJSON_IsArray(board) || cJSON_GetArraySize(board) > 128) {
      cJSON_Delete(board);
      board = NULL;
    }
  }
  free(manifest);
  free(artifact);
  free(board_json);
  return e;
}
bool gw_authority_has(const char *cap) {
  return cap && contains(authorized, cap);
}
bool gw_authority_board_verified(const char *cap) {
  return gw_authority_has(cap) && contains(board, cap);
}
const char *gw_authority_digest(void) { return manifest_digest; }
cJSON *gw_authority_factory_image(const char *component) {
  if (!component ||
      (strcmp(component, "GATEWAY") && strcmp(component, "NRF54")))
    return NULL;
  char *raw = calloc(1, 16385), *keys = calloc(1, 8192), model[129],
       hardware[129];
  cJSON *bundle = NULL, *m = NULL, *trusted = NULL, *proof = NULL;
  if (!raw || !keys)
    goto done;
  if (factory_text(!strcmp(component, "GATEWAY") ? "gw_fwproof" : "nrf_fwproof",
                   raw, 16385) != ESP_OK ||
      gw_factory_get_string("artifact_keys", keys, 8192) != ESP_OK ||
      gw_factory_get_string("model", model, sizeof(model)) != ESP_OK ||
      gw_factory_get_string("hardware", hardware, sizeof(hardware)) != ESP_OK)
    goto done;
  bundle = cJSON_Parse(raw);
  trusted = cJSON_Parse(keys);
  const char *manifest = s(bundle, "manifest");
  if (!*manifest || strlen(manifest) > 8192 || strstr(manifest, "\\u0000"))
    goto done;
  const char *end = NULL;
  m = cJSON_ParseWithLengthOpts(manifest, strlen(manifest) + 1, &end, true);
  if (!cJSON_IsObject(m) || !unique(bundle, 0) || !unique(m, 0) ||
      !unique(trusted, 0))
    goto done;
  const char *public = s(trusted, s(m, "keyId")),
             *signature = s(bundle, "signature");
  uint8_t pk[32], sig[64];
  size_t pn, sn;
  if (mbedtls_base64_decode(pk, 32, &pn, (const uint8_t *)public,
                            strlen(public)) ||
      pn != 32 ||
      mbedtls_base64_decode(sig, 64, &sn, (const uint8_t *)signature,
                            strlen(signature)) ||
      sn != 64 ||
      crypto_ed25519_check(sig, pk, (const uint8_t *)manifest,
                           strlen(manifest)))
    goto done;
  const char *target = s(m, "component");
  if (!*target)
    target = "GATEWAY";
  const cJSON *size = cJSON_GetObjectItemCaseSensitive(m, "sizeBytes"),
              *hw = cJSON_GetObjectItemCaseSensitive(m, "hardwareVersions");
  if (strcmp(s(m, "kind"), "FIRMWARE") || strcmp(target, component) ||
      !*s(m, "version") || strlen(s(m, "version")) > 64 ||
      strlen(s(m, "sha256")) != 64 ||
      !contains(cJSON_GetObjectItemCaseSensitive(m, "compatibleModels"),
                model) ||
      (hw && cJSON_GetArraySize(hw) && !contains(hw, hardware)) ||
      !cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
      size->valuedouble < 32 || size->valuedouble > UINT32_MAX ||
      floor(size->valuedouble) != size->valuedouble ||
      (!strcmp(component, "NRF54") &&
       strcmp(s(m, "imageFormat"), "MCUBOOT_SIGNED_BIN")))
    goto done;
  proof = cJSON_CreateObject();
  cJSON_AddStringToObject(proof, "component", component);
  cJSON_AddStringToObject(proof, "source", "FACTORY");
  cJSON_AddStringToObject(proof, "phase", "CONFIRMED");
  cJSON_AddStringToObject(proof, "version", s(m, "version"));
  cJSON_AddStringToObject(proof, "sha256", s(m, "sha256"));
  cJSON_AddNumberToObject(proof, "size", size->valuedouble);
  if (*s(bundle, "artifactId"))
    cJSON_AddStringToObject(proof, "artifactId", s(bundle, "artifactId"));
done:
  free(raw);
  free(keys);
  cJSON_Delete(bundle);
  cJSON_Delete(m);
  cJSON_Delete(trusted);
  return proof;
}
