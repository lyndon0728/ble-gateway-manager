#include "gw_sync.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "gw_authority.h"
#include "gw_capture.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_nrf.h"
#include "../gw_core/gw_rules.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *s(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static bool u64(const char *s, uint64_t *v) {
  *v = 0;
  if (!s || !*s || (*s == '0' && s[1]))
    return false;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || *v > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
      return false;
    *v = *v * 10 + (unsigned)(*s - '0');
  }
  return true;
}
static void hex(const uint8_t *p, size_t n, char *out) {
  static const char h[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = h[p[i] >> 4];
    out[i * 2 + 1] = h[p[i] & 15];
  }
  out[n * 2] = 0;
}
static bool unhex(const char *s, uint8_t *out, size_t n) {
  if (strlen(s) != 2 * n)
    return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v = 0;
    for (unsigned j = 0; j < 2; j++) {
      char c = s[2 * i + j];
      if (c >= '0' && c <= '9')
        v = (v << 4) | (c - '0');
      else if (c >= 'a' && c <= 'f')
        v = (v << 4) | (c - 'a' + 10);
      else
        return false;
    }
    out[i] = v;
  }
  return true;
}
static cJSON *identity(const char *ns, const char *gw) {
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "schemaVersion", "1.0");
  cJSON_AddStringToObject(j, "namespaceId", ns);
  cJSON_AddStringToObject(j, "gatewayId", gw);
  return j;
}
static bool known(const cJSON *j, const char *const *keys, size_t count) {
  if (!cJSON_IsObject(j))
    return false;
  for (const cJSON *v = j->child; v; v = v->next) {
    bool found = false;
    for (size_t i = 0; i < count; i++)
      if (!strcmp(v->string, keys[i]))
        found = true;
    if (!found)
      return false;
    for (const cJSON *n = v->next; n; n = n->next)
      if (!strcmp(n->string, v->string))
        return false;
  }
  return true;
}
static bool revision(const cJSON *v) {
  return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
         v->valuedouble <= 9007199254740991.0 &&
         floor(v->valuedouble) == v->valuedouble;
}
static const char *const groups[] = {"network",          "nrf54Scan",
                                     "nrf54Advertising", "esp32Scan",
                                     "esp32Advertising", "upload"};
static bool unique_subset(const cJSON *array, const char *const *allowed,
                          size_t n) {
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > (int)n)
    return false;
  for (const cJSON *v = array->child; v; v = v->next) {
    if (!cJSON_IsString(v))
      return false;
    bool found = false;
    for (size_t i = 0; i < n; i++)
      if (!strcmp(v->valuestring, allowed[i]))
        found = true;
    if (!found)
      return false;
    for (const cJSON *x = v->next; x; x = x->next)
      if (cJSON_IsString(x) && !strcmp(x->valuestring, v->valuestring))
        return false;
  }
  return true;
}
static bool complete_ipv4(const cJSON *value) {
  static const char *const keys[]={"mode","address","prefixLength","gateway","dns"};
  if(!known(value,keys,5)) return false;
  if(!strcmp(s(value,"mode"),"DHCP")) return cJSON_GetArraySize(value)==1;
  const cJSON *prefix=cJSON_GetObjectItemCaseSensitive(value,"prefixLength"),
              *dns=cJSON_GetObjectItemCaseSensitive(value,"dns");
  if(strcmp(s(value,"mode"),"STATIC")||cJSON_GetArraySize(value)!=5||
     !*s(value,"address")||!*s(value,"gateway")||!revision(prefix)||
     prefix->valuedouble<1||prefix->valuedouble>30||!cJSON_IsArray(dns)||
     cJSON_GetArraySize(dns)<1||cJSON_GetArraySize(dns)>2) return false;
  uint32_t address,gateway,mask=UINT32_MAX<<(32-(unsigned)prefix->valuedouble);
  if(!gw_rules_ipv4(s(value,"address"),&address)||
     !gw_rules_ipv4(s(value,"gateway"),&gateway)||address==gateway||
     (address&mask)!=(gateway&mask)||!(address&~mask)||
     (address&~mask)==~mask||!(gateway&~mask)||(gateway&~mask)==~mask)return false;
  for(const cJSON *v=dns->child;v;v=v->next) {
    uint32_t ip;
    if(!cJSON_IsString(v)||!gw_rules_ipv4(v->valuestring,&ip))return false;
  }
  return true;
}
static bool advertising_data(const cJSON *value,const char *key) {
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(value,key);
  if(!cJSON_IsString(v)||strlen(v->valuestring)>44)return false;
  uint8_t data[31];size_t n=0;
  if(mbedtls_base64_decode(data,sizeof(data),&n,(const uint8_t *)v->valuestring,
                          strlen(v->valuestring)))return false;
  for(size_t p=0;p<n;) {
    if(!data[p]){for(;p<n;p++)if(data[p])return false;return true;}
    if(data[p]>n-p-1)return false;
    p+=data[p]+1;
  }
  return true;
}
static bool group_value(const char *name, const cJSON *value) {
  if (!cJSON_IsObject(value))
    return false;
  if (strstr(name, "Scan")) {
    const char *const fields[] = {"scan", "filters", "beaconRules",
                                  "beaconFilterV2"};
    if (!known(value, fields, 4) || cJSON_GetArraySize(value) != 4)
      return false;
    const cJSON *scan = cJSON_GetObjectItemCaseSensitive(value, "scan"),
                *filters = cJSON_GetObjectItemCaseSensitive(value, "filters");
    if (!cJSON_HasObjectItem(scan, "enabled") ||
        !cJSON_HasObjectItem(scan, "active") ||
        !cJSON_HasObjectItem(scan, "intervalMs") ||
        !cJSON_HasObjectItem(scan, "windowMs") ||
        !cJSON_HasObjectItem(filters, "minRssi") ||
        !cJSON_HasObjectItem(filters, "serviceUuids") ||
        !cJSON_HasObjectItem(filters, "manufacturerIds"))
      return false;
    uint8_t *wire = malloc(3070);
    size_t n = 0;
    bool valid =
        /* Validation uses a legal synthetic revision; historical factory
         * revisions may be zero, but no radio configuration is applied here. */
        wire && gw_nrf_compile_scan(value, 1, wire, 3070, &n) == ESP_OK;
    free(wire);
    return valid;
  }
  if (!strcmp(name, "upload"))
    return gw_capture_upload_valid(value);
  if (!strcmp(name, "network")) {
    static const char *const top[]={"uplinkOrder","ethernet","wifiSta"},
                             *const eth_keys[]={"enabled","ipv4"},
                             *const wifi_keys[]={"enabled","ipv4","ssid","security","credentialRef"},
                             *const uplinks[]={"ETHERNET","WIFI_STA"};
    const cJSON *order=cJSON_GetObjectItemCaseSensitive(value,"uplinkOrder"),
                *eth=cJSON_GetObjectItemCaseSensitive(value,"ethernet"),
                *wifi=cJSON_GetObjectItemCaseSensitive(value,"wifiSta");
    if(!known(value,top,3)||cJSON_GetArraySize(value)!=3||
       !known(eth,eth_keys,2)||!known(wifi,wifi_keys,5)||
       !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(eth,"enabled"))||
       !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(wifi,"enabled"))||
       !complete_ipv4(cJSON_GetObjectItemCaseSensitive(eth,"ipv4"))||
       !complete_ipv4(cJSON_GetObjectItemCaseSensitive(wifi,"ipv4"))||
       !unique_subset(order,uplinks,2)||!cJSON_GetArraySize(order))return false;
    for(unsigned i=2;i<5;i++) {
      const cJSON *field=cJSON_GetObjectItemCaseSensitive(wifi,wifi_keys[i]);
      if(field&&!cJSON_IsString(field))return false;
    }
    if(cJSON_HasObjectItem(wifi,"ssid")&&
       (!gw_rules_utf8(s(wifi,"ssid"))||strlen(s(wifi,"ssid"))>32))return false;
    if(cJSON_HasObjectItem(wifi,"security")&&strcmp(s(wifi,"security"),"WPA2_PSK")&&
       strcmp(s(wifi,"security"),"WPA3_SAE"))return false;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wifi,"enabled"))&&
       (!*s(wifi,"ssid")||strlen(s(wifi,"ssid"))>32||!*s(wifi,"credentialRef")||
        (strcmp(s(wifi,"security"),"WPA2_PSK")&&strcmp(s(wifi,"security"),"WPA3_SAE"))))return false;
    for(const cJSON *v=order->child;v;v=v->next)
      if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(!strcmp(v->valuestring,"ETHERNET")?eth:wifi,"enabled")))return false;
    return true;
  }
  static const char *const adv_keys[]={"enabled","mode","intervalMs","advDataBase64","scanResponseBase64"};
  const cJSON *interval=cJSON_GetObjectItemCaseSensitive(value,"intervalMs");
  return known(value,adv_keys,5)&&cJSON_GetArraySize(value)==5&&
         cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(value,"enabled"))&&
         (!strcmp(s(value,"mode"),"NONCONNECTABLE")||
          !strcmp(s(value,"mode"),"SCANNABLE_NONCONNECTABLE")||
          !strcmp(s(value,"mode"),"CONNECTABLE_SCANNABLE"))&&
         revision(interval)&&interval->valuedouble>=100&&interval->valuedouble<=10240&&
         advertising_data(value,"advDataBase64")&&advertising_data(value,"scanResponseBase64")&&
         (strcmp(s(value,"mode"),"NONCONNECTABLE")||!*s(value,"scanResponseBase64"));
}
esp_err_t gw_cloud_snapshot_check(const cJSON *snap, const char *ns,
                                  const char *gw) {
  static const char *const top[] = {
      "schemaVersion",  "namespaceId", "gatewayId", "snapshotId",
      "generation",     "complete",    "groups",    "missingGroups",
      "blockedReasons", "digest"};
  static const char *const reasons[] = {"UNKNOWN_TASK", "LOCAL_OVERRIDE",
                                        "MISSING_BASELINE",
                                        "PENDING_CONFIGURATION"};
  uint64_t generation;
  if (!known(snap, top, 10) || cJSON_GetArraySize(snap) != 10 ||
      strcmp(s(snap, "schemaVersion"), "1.0") ||
      strcmp(s(snap, "namespaceId"), ns) || strcmp(s(snap, "gatewayId"), gw) ||
      !*s(snap, "snapshotId") || strlen(s(snap, "snapshotId")) > 128 ||
      !u64(s(snap, "generation"), &generation) || !generation ||
      !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(snap, "complete")))
    return ESP_ERR_INVALID_RESPONSE;
  cJSON *copy = cJSON_Duplicate(snap, true);
  if (!copy)
    return ESP_ERR_NO_MEM;
  cJSON_DeleteItemFromObjectCaseSensitive(copy, "digest");
  char digest[65];
  esp_err_t e = gw_core_network_snapshot_id(copy, digest);
  cJSON_Delete(copy);
  if (e != ESP_OK || strcmp(s(snap, "digest"), digest))
    return ESP_ERR_INVALID_RESPONSE;
  const cJSON *values = cJSON_GetObjectItemCaseSensitive(snap, "groups"),
              *missing =
                  cJSON_GetObjectItemCaseSensitive(snap, "missingGroups"),
              *blocked =
                  cJSON_GetObjectItemCaseSensitive(snap, "blockedReasons");
  if (!known(values, groups, 6) || !unique_subset(missing, groups, 6) ||
      !unique_subset(blocked, reasons, 4))
    return ESP_ERR_INVALID_RESPONSE;
  bool complete =
      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snap, "complete"));
  if (complete && (cJSON_GetArraySize(missing) || cJSON_GetArraySize(blocked) ||
                   cJSON_GetArraySize(values) != 6))
    return ESP_ERR_INVALID_RESPONSE;
  for (size_t i = 0; i < 6; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, groups[i]);
    bool absent = false;
    for (const cJSON *m = missing->child; m; m = m->next)
      if (!strcmp(m->valuestring, groups[i]))
        absent = true;
    if ((v != NULL) == absent)
      return ESP_ERR_INVALID_RESPONSE;
    if (!v)
      continue;
    static const char *const group_keys[] = {"revision", "value", "source",
                                             "verifiedAt"},
                             *const source_keys[] = {"kind", "id"};
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(v, "source");
    if (!known(v, group_keys, 4) || cJSON_GetArraySize(v) != 4 ||
        !known(source, source_keys, 2) || cJSON_GetArraySize(source) != 2 ||
        !revision(cJSON_GetObjectItemCaseSensitive(v, "revision")) ||
        !*s(v, "verifiedAt") || !*s(source, "id") ||
        (strcmp(s(source, "kind"), "VERIFIED_TASK") &&
         strcmp(s(source, "kind"), "FACTORY_MANIFEST") &&
         strcmp(s(source, "kind"), "LOCAL_ADOPTED")) ||
        !group_value(groups[i], cJSON_GetObjectItemCaseSensitive(v, "value")))
      return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}
static void snapshot(const char *ns, const char *gw) {
  cJSON *j = NULL;
  if (gw_http_json("/api/v1/gateway-configuration-snapshot", "GET", NULL, NULL,
                   gw_core_mono_ms() + 5000, &j) == ESP_OK &&
      gw_cloud_snapshot_check(j, ns, gw) == ESP_OK) {
    cJSON *old = gw_store_get_json("cloud/snapshotView");
    uint64_t prev = 0, next = 0;
    u64(s(old, "generation"), &prev);
    u64(s(j, "generation"), &next);
    if (next > prev ||
        (next == prev && !strcmp(s(old, "digest"), s(j, "digest")))) {
      if (gw_store_put_json("cloud/snapshotView", j) == ESP_OK &&
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "complete")))
        (void)gw_store_put_json("cloud/recoverySnapshot", j);
    }
    cJSON_Delete(old);
  }
  cJSON_Delete(j);
}
esp_err_t gw_cloud_recovery_check(const cJSON *command, const char *ns,
                                  const char *gw, uint64_t deadline) {
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(command, "parameters");
  const char *id = s(p, "sourceSnapshotId");
  if (!*id)
    return ESP_OK;
  if (!gw_authority_has("CONFIG_SNAPSHOT_1") || strlen(id) > 128)
    return ESP_ERR_INVALID_STATE;
  for (const char *q = id; *q; q++)
    if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
          (*q >= '0' && *q <= '9') || *q == '_' || *q == '-' || *q == '.' ||
          *q == ':'))
      return ESP_ERR_INVALID_ARG;
  cJSON *snap = gw_store_get_json("cloud/recoverySnapshot");
  if (strcmp(s(snap, "snapshotId"), id)) {
    cJSON_Delete(snap);
    snap = NULL;
    char path[256];
    snprintf(path, sizeof(path),
             "/api/v1/gateway-configuration-snapshot?snapshotId=%s", id);
    if (gw_http_json(path, "GET", NULL, NULL, deadline, &snap) != ESP_OK) {
      cJSON_Delete(snap);
      return ESP_ERR_INVALID_STATE;
    }
  }
  esp_err_t e = gw_cloud_snapshot_check(snap, ns, gw);
  if (e != ESP_OK || strcmp(s(snap, "snapshotId"), id) ||
      !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snap, "complete"))) {
    cJSON_Delete(snap);
    return ESP_ERR_INVALID_STATE;
  }
  const cJSON *patch = cJSON_GetObjectItemCaseSensitive(p, "configuration"),
              *value = NULL;
  const char *group = NULL;
  if (cJSON_IsObject(patch) && cJSON_GetArraySize(patch) == 1) {
    value = cJSON_GetObjectItemCaseSensitive(patch, "network");
    if (value)
      group = "network";
    if (!value) {
      value = cJSON_GetObjectItemCaseSensitive(patch, "upload");
      if (value)
        group = "upload";
    }
    if (!value) {
      const cJSON *radios = cJSON_GetObjectItemCaseSensitive(patch, "radios");
      if (cJSON_IsObject(radios) && cJSON_GetArraySize(radios) == 1) {
        const cJSON *radio = radios->child;
        bool nrf = !strcmp(radio->string, "nrf54");
        if (nrf || !strcmp(radio->string, "esp32")) {
          const cJSON *adv =
              cJSON_GetObjectItemCaseSensitive(radio, "advertising");
          if (adv && cJSON_GetArraySize(radio) == 1) {
            value = adv;
            group = nrf ? "nrf54Advertising" : "esp32Advertising";
          } else if (!adv) {
            value = radio;
            group = nrf ? "nrf54Scan" : "esp32Scan";
          }
        }
      }
    }
  }
  const cJSON *entry =
      group ? cJSON_GetObjectItemCaseSensitive(
                  cJSON_GetObjectItemCaseSensitive(snap, "groups"), group)
            : NULL;
  const cJSON *rev = cJSON_GetObjectItemCaseSensitive(p, "desiredRevision"),
              *old_rev = cJSON_GetObjectItemCaseSensitive(entry, "revision");
  e = value && entry && revision(rev) && revision(old_rev) &&
              rev->valuedouble > old_rev->valuedouble &&
              cJSON_Compare(
                  value, cJSON_GetObjectItemCaseSensitive(entry, "value"), true)
          ? ESP_OK
          : ESP_ERR_INVALID_ARG;
  cJSON *state = gw_core_state();
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "localOverride")))
    e = ESP_ERR_INVALID_STATE;
  cJSON_Delete(state);
  if (e == ESP_OK)
    e = gw_store_put_json("cloud/recoverySnapshot", snap);
  cJSON_Delete(snap);
  return e;
}
static bool actual_image(const char *component, const cJSON *proof,
                         uint64_t deadline, uint8_t boot[16],
                         char version[64]) {
  uint8_t expected[32];
  const cJSON *size = cJSON_GetObjectItemCaseSensitive(proof, "size");
  if (strcmp(s(proof, "phase"), "CONFIRMED") ||
      strcmp(s(proof, "component"), component) ||
      !unhex(s(proof, "sha256"), expected, 32) || !cJSON_IsNumber(size) ||
      size->valuedouble < 32 || size->valuedouble > UINT32_MAX ||
      floor(size->valuedouble) != size->valuedouble)
    return false;
  if (!strcmp(component, "NRF54")) {
    uint32_t caps;
    uint8_t after[16], status, data[62], args[36];
    char after_version[64];
    size_t n = sizeof(data);
    if (!gw_nrf_identity(boot, &caps, version) || !(caps & (1u << 12)) ||
        strcmp(version, s(proof, "version")))
      return false;
    gw_put_u32(args, (uint32_t)size->valuedouble);
    memcpy(args + 4, expected, 32);
    n = 0;
    if (gw_nrf_command(0x37, args, sizeof(args), &status, NULL, &n, deadline) !=
            ESP_OK ||
        status || n)
      return false;
    n = sizeof(data);
    return gw_nrf_command(0x33, NULL, 0, &status, data, &n, deadline) ==
               ESP_OK &&
           !status && n == 62 && data[8] == 0 && data[9] == 1 &&
           gw_nrf_identity(after, NULL, after_version) &&
           !memcmp(boot, after, 16) && !strcmp(version, after_version);
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running ||
      (*s(proof, "partition") &&
       strcmp(running->label, s(proof, "partition"))) ||
      size->valuedouble > running->size ||
      esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_VALID ||
      strcmp(esp_app_get_description()->version, s(proof, "version")))
    return false;
  uint8_t *buffer = malloc(4096), hash[32];
  if (!buffer)
    return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  int rc = mbedtls_sha256_starts(&ctx, 0);
  for (size_t pos = 0; !rc && pos < (size_t)size->valuedouble;) {
    size_t take = (size_t)size->valuedouble - pos;
    if (take > 4096)
      take = 4096;
    if (gw_core_mono_ms() >= deadline ||
        esp_partition_read(running, pos, buffer, take) != ESP_OK) {
      rc = -1;
      break;
    }
    rc = mbedtls_sha256_update(&ctx, buffer, take);
    pos += take;
  }
  if (!rc)
    rc = mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  free(buffer);
  return !rc && !memcmp(hash, expected, 32);
}
static void inventory(const char *component, const char *ns, const char *gw,
                      const char *esp_boot) {
  if (gw_core_maintenance_acquire("component-inventory") != ESP_OK)
    return;
  uint64_t deadline = gw_core_mono_ms() + 15000;
  cJSON *request = cJSON_CreateObject(), *challenge = NULL, *report = NULL,
        *ack = NULL, *proof = NULL;
  cJSON_AddStringToObject(request, "component", component);
  if (gw_http_json("/api/v1/gateway-component-challenges", "POST", request,
                   NULL, deadline, &challenge) != ESP_OK)
    goto done;
  uint64_t expiry, generation;
  if (strcmp(s(challenge, "component"), component) ||
      !*s(challenge, "challengeId") || strlen(s(challenge, "nonce")) != 44 ||
      !u64(s(challenge, "inventoryGeneration"), &generation) ||
      !u64(s(challenge, "expiresAtUnixMs"), &expiry))
    goto done;
  uint64_t limit = gw_core_time_deadline(expiry);
  if (deadline > limit)
    deadline = limit;
  if (deadline <= gw_core_mono_ms())
    goto done;
  report = identity(ns, gw);
  const char *keys[] = {"challengeId", "nonce", "inventoryGeneration",
                        "component"};
  for (unsigned i = 0; i < 4; i++)
    cJSON_AddStringToObject(report, keys[i], s(challenge, keys[i]));
  uint8_t boot[16] = {0};
  char version[64] = {0}, boot_hex[33];
  bool available;
  if (!strcmp(component, "GATEWAY")) {
    available = true;
    snprintf(version, sizeof(version), "%s",
             esp_app_get_description()->version);
    snprintf(boot_hex, sizeof(boot_hex), "%s", esp_boot);
  } else {
    available = gw_nrf_identity(boot, NULL, version);
    hex(boot, 16, boot_hex);
  }
  bool confirmed = false;
  /* Proof reads may overwrite their output buffers even when they fail.
   * Retain the original identity so a reboot cannot create mixed evidence. */
  uint8_t initial_boot[16];
  char initial_version[64];
  memcpy(initial_boot,boot,sizeof(initial_boot));
  snprintf(initial_version,sizeof(initial_version),"%s",version);
  char proof_key[32];
  snprintf(proof_key, sizeof(proof_key), "ota/proof/%s", component);
  proof = gw_store_get_json(proof_key);
  if (!proof)
    proof = gw_authority_factory_image(component);
  if (available && actual_image(component, proof, deadline, boot, version)) {
    if (!strcmp(component, "NRF54"))
      hex(boot, 16, boot_hex);
    if (!*s(proof, "artifactId")) {
      char path[192];
      snprintf(path, sizeof(path),
               "/api/v1/gateway-component-artifact?component=%s&sha256=%s",
               component, s(proof, "sha256"));
      cJSON *artifact = NULL;
      if (gw_http_json(path, "GET", NULL, NULL, deadline, &artifact) ==
              ESP_OK &&
          !strcmp(s(artifact, "component"), component) &&
          !strcmp(s(artifact, "imageSha256"), s(proof, "sha256")) &&
          !strcmp(s(artifact, "firmwareVersion"), version) &&
          *s(artifact, "artifactId")) {
        cJSON_DeleteItemFromObjectCaseSensitive(proof, "artifactId");
        cJSON_AddStringToObject(proof, "artifactId", s(artifact, "artifactId"));
      }
      cJSON_Delete(artifact);
    }
    confirmed = *s(proof, "artifactId") != 0;
  }
  /* A boot change during a failed proof must also invalidate VERSION_ONLY. */
  if (!strcmp(component, "NRF54")) {
    uint8_t after[16];
    char after_version[64];
    if (!gw_nrf_identity(after, NULL, after_version) ||
        memcmp(after, boot, 16) || strcmp(after_version, version) ||
        memcmp(after,initial_boot,16) || strcmp(after_version,initial_version)) {
      available = false;
      confirmed = false;
    }
  }
  cJSON_AddStringToObject(report, "evidenceLevel",
                          confirmed   ? "CONFIRMED_ARTIFACT"
                          : available ? "VERSION_ONLY"
                                      : "UNKNOWN");
  if (available) {
    cJSON_AddStringToObject(report, "controllerBootId", boot_hex);
    cJSON_AddStringToObject(report, "firmwareVersion", version);
  }
  if (confirmed) {
    cJSON_AddStringToObject(report, "imageSha256", s(proof, "sha256"));
    cJSON_AddStringToObject(report, "artifactId", s(proof, "artifactId"));
    cJSON_AddBoolToObject(report, "bootConfirmed", true);
  }
  uint64_t receipt_generation;
  if (gw_core_mono_ms() < deadline &&
      gw_http_json("/api/v1/gateway-components", "PUT", report, NULL, deadline,
                   &ack) == ESP_OK &&
      !strcmp(s(ack, "namespaceId"), ns) && !strcmp(s(ack, "gatewayId"), gw) &&
      !strcmp(s(ack, "component"), component) &&
      !strcmp(s(ack,"schemaVersion"),"1.0") &&
      !strcmp(s(ack,"challengeId"),s(challenge,"challengeId")) &&
      generation!=UINT64_MAX && u64(s(ack,"inventoryGeneration"),&receipt_generation) &&
      receipt_generation==generation+1) {
    char key[40];
    snprintf(key, sizeof(key), "cloud/component/%s", component);
    (void)gw_store_put_json(key, ack);
  }
done:
  cJSON_Delete(request);
  cJSON_Delete(challenge);
  cJSON_Delete(report);
  cJSON_Delete(ack);
  cJSON_Delete(proof);
  gw_core_maintenance_release("component-inventory");
}
static void capabilities(const char *ns, const char *gw, const char *esp_boot) {
  if (!*gw_authority_digest())
    return;
  uint8_t boot[16];
  uint32_t bits = 0;
  bool live = gw_nrf_identity(boot, &bits, NULL);
  char nrf_boot[33];
  if (live)
    hex(boot, 16, nrf_boot);
  cJSON *j = identity(ns, gw),
        *implemented = cJSON_AddArrayToObject(j, "espImplemented"),
        *hello = cJSON_AddArrayToObject(j, "nrfHelloCapabilities"),
        *verified = cJSON_AddArrayToObject(j, "boardVerified"),
        *reasons = cJSON_AddObjectToObject(j, "unavailableReasons"),
        *ack = NULL;
  cJSON_AddStringToObject(j, "espBootId", esp_boot);
  if (live)
    cJSON_AddStringToObject(j, "nrfBootId", nrf_boot);
  else
    cJSON_AddNullToObject(j, "nrfBootId");
  cJSON_AddStringToObject(j, "manifestDigest", gw_authority_digest());
  static const struct {
    const char *name;
    uint32_t nrf_bits;
  } all[] = {{"CONFIGURE", 0},
             {"REBOOT", 0},
             {"DIAGNOSTICS", 0},
             {"FIRMWARE_UPGRADE", 0},
             {"GATT_CONNECT", 0},
             {"GATT_DISCONNECT", 0},
             {"GATT_READ", 0},
             {"GATT_WRITE", 0},
             {"GATT_SUBSCRIBE", 0},
             {"GATT_UNSUBSCRIBE", 0},
             {"PERIPHERAL_NOTIFY", 0},
             {"PERIPHERAL_DISCONNECT", 0},
             {"GW32_CONTROL_PROFILE_1", 0},
             {"GW32_CAPTURE_CONTEXT_1", 0},
             {"NETWORK_WIFI_STA", 0},
             {"NETWORK_ETHERNET", 0},
             {"COMPONENT_INVENTORY_1", 0},
             {"CONFIG_SNAPSHOT_1", 0},
             {"SIGNED_TIME_1", 0},
             {"PERIPHERAL_BRIDGE_1", 0},
             {"NRF54_UART_DFU_1", 1u << 12},
             {"NRF54_BLE_SCAN", 1u << 1},
      {"NRF54_BLE_ADV", 1u << 4},
             {"NRF54_BLE_BEACON_FILTER", 1u << 1},
             {"NRF54_BLE_FILTER_V2", (1u << 1) | (1u << 13)},
             {"NRF54_BLE_GATT_CENTRAL", 1u << 2},
             {"NRF54_BLE_GATT_NOTIFY", 1u << 3},
      {"NRF54_BLE_GATT_SERVICE_DISCOVERY", (1u << 2) | (1u << 9)},
      {"NRF54_BLE_PERIPHERAL", (1u << 4) | (1u << 11)},
             {"ESP32_BLE_SCAN", 0},
             {"ESP32_BLE_ADV", 0},
             {"ESP32_BLE_BEACON_FILTER", 0},
             {"ESP32_BLE_FILTER_V2", 0},
             {"ESP32_BLE_GATT_CENTRAL", 0},
             {"ESP32_BLE_GATT_NOTIFY", 0},
             {"ESP32_BLE_GATT_SERVICE_DISCOVERY", 0},
             {"ESP32_BLE_PERIPHERAL", 0}};
  for (unsigned i = 0; i < sizeof(all) / sizeof(*all); i++) {
    const char *cap = all[i].name;
    if (!gw_authority_has(cap))
      continue;
    bool built = true;
#if !CONFIG_GW_ETH_ENABLED
    if (!strcmp(cap, "NETWORK_ETHERNET"))
      built = false;
#endif
    if (built)
      cJSON_AddItemToArray(implemented, cJSON_CreateString(cap));
    bool peer =
        all[i].nrf_bits && live && (bits & all[i].nrf_bits) == all[i].nrf_bits;
    if (peer)
      cJSON_AddItemToArray(hello, cJSON_CreateString(cap));
    if (gw_authority_board_verified(cap))
      cJSON_AddItemToArray(verified, cJSON_CreateString(cap));
    if (!built)
      cJSON_AddStringToObject(reasons, cap, "NOT_IMPLEMENTED");
    else if (all[i].nrf_bits && !peer)
      cJSON_AddStringToObject(reasons, cap, "NRF_CAPABILITY_ABSENT");
    else if (!gw_authority_board_verified(cap))
      cJSON_AddStringToObject(reasons, cap, "BOARD_NOT_VERIFIED");
  }
  (void)gw_http_json("/api/v1/gateway-runtime-capabilities", "PUT", j, NULL,
                     gw_core_mono_ms() + 5000, &ack);
  cJSON_Delete(ack);
  cJSON_Delete(j);
}
void gw_cloud_sync(const char *ns, const char *gw, const char *esp_boot) {
  capabilities(ns, gw, esp_boot);
  if (gw_authority_has("COMPONENT_INVENTORY_1")) {
    inventory("GATEWAY", ns, gw, esp_boot);
    inventory("NRF54", ns, gw, esp_boot);
  }
  if (gw_authority_has("CONFIG_SNAPSHOT_1"))
    snapshot(ns, gw);
}
