#include "gw_http.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "gw_core.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define JSON_LIMIT (128U * 1024U)
typedef struct {
  char *bytes;
  size_t length, capacity, limit;
  uint64_t deadline;
  gw_http_sink_fn sink;
  void *context;
  esp_err_t failure;
} response_t;
static int budget(uint64_t deadline) {
  uint64_t now = gw_core_mono_ms();
  if (deadline <= now)
    return 0;
  return deadline - now > 10000 ? 10000 : (int)(deadline - now);
}
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static bool skip_dns_name(const uint8_t *p, size_t n, size_t *off) {
  unsigned labels = 0;
  while (*off < n && ++labels < 128) {
    uint8_t l = p[(*off)++];
    if (!l)
      return true;
    if ((l & 0xc0) == 0xc0) {
      if (*off >= n)
        return false;
      unsigned ptr = ((l & 63) << 8) | p[(*off)++];
      return ptr < n;
    }
    if ((l & 0xc0) || l > 63 || l > n - *off)
      return false;
    *off += l;
  }
  return false;
}
/* A fresh connected UDP socket is bound to the candidate source AND netif.
 * No lwIP global resolver cache/default-route proof is used for candidates. */
static esp_err_t resolve(esp_netif_t *netif, const char *host,
                         struct in_addr *out, struct ifreq *iface,
                         uint64_t deadline) {
  if (!netif || esp_netif_get_netif_impl_name(netif, iface->ifr_name) != ESP_OK)
    return ESP_ERR_INVALID_STATE;
  if (inet_aton(host, out))
    return ESP_OK;
  esp_netif_ip_info_t ip;
  esp_netif_dns_info_t dns;
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || !ip.ip.addr ||
      esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
      dns.ip.type != ESP_IPADDR_TYPE_V4 || !dns.ip.u_addr.ip4.addr)
    return ESP_ERR_INVALID_STATE;
  uint8_t q[272] = {0}, r[1536];
  unsigned id = esp_random() & 65535;
  q[0] = id >> 8;
  q[1] = id;
  q[2] = 1;
  q[5] = 1;
  size_t n = 12;
  const char *start = host;
  while (*start) {
    const char *dot = strchr(start, '.');
    size_t l = dot ? (size_t)(dot - start) : strlen(start);
    if (!l || l > 63 || n + l + 6 > sizeof(q))
      return ESP_ERR_INVALID_ARG;
    q[n++] = l;
    memcpy(q + n, start, l);
    n += l;
    if (!dot)
      break;
    start = dot + 1;
  }
  q[n++] = 0;
  q[n++] = 0;
  q[n++] = 1;
  q[n++] = 0;
  q[n++] = 1;
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return ESP_FAIL;
  struct sockaddr_in local = {.sin_family = AF_INET,
                              .sin_addr.s_addr = ip.ip.addr};
  struct sockaddr_in dest = {.sin_family = AF_INET,
                             .sin_port = htons(53),
                             .sin_addr.s_addr = dns.ip.u_addr.ip4.addr};
  int ms = budget(deadline);
  if (ms > 3000)
    ms = 3000;
  struct timeval timeout = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
  esp_err_t e = ESP_FAIL;
  if (!ms ||
      setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, sizeof(*iface)) ||
      bind(fd, (void *)&local, sizeof(local)) ||
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
      connect(fd, (void *)&dest, sizeof(dest)))
    goto done;
  if (send(fd, q, n, 0) != (int)n)
    goto done;
  int len = recv(fd, r, sizeof(r), 0);
  if (len < (int)n || be16(r) != id || (r[2] & 0x82) != 0x80 || (r[3] & 15) ||
      be16(r + 4) != 1 || memcmp(r + 12, q + 12, n - 12))
    goto done;
  size_t pos = n;
  unsigned count = be16(r + 6);
  if (count > 128)
    goto done;
  for (unsigned i = 0; i < count; i++) {
    if (!skip_dns_name(r, len, &pos) || pos + 10 > (size_t)len)
      goto done;
    unsigned type = be16(r + pos), cls = be16(r + pos + 2),
             rdlen = be16(r + pos + 8);
    pos += 10;
    if (rdlen > (size_t)len - pos)
      goto done;
    if (type == 1 && cls == 1 && rdlen == 4) {
      memcpy(&out->s_addr, r + pos, 4);
      e = ESP_OK;
      goto done;
    }
    pos += rdlen;
  }
done:
  close(fd);
  return e;
}
static esp_err_t http_event(esp_http_client_event_t *ev) {
  response_t *r = ev->user_data;
  if (!r)
    return ESP_OK;
  if (r->failure != ESP_OK)
    return r->failure;
  if (!budget(r->deadline)) {
    r->failure = ESP_ERR_TIMEOUT;
    return r->failure;
  }
  if (ev->event_id != HTTP_EVENT_ON_DATA || ev->data_len <= 0)
    return ESP_OK;
  int status = esp_http_client_get_status_code(ev->client);
  if (status < 200 || status >= 300) {
    r->failure = ESP_FAIL;
    return ESP_FAIL;
  }
  if ((size_t)ev->data_len > r->limit - r->length) {
    r->failure = ESP_ERR_INVALID_SIZE;
    return r->failure;
  }
  if (r->sink) {
    r->failure = r->sink(ev->data, ev->data_len, r->context);
    if (r->failure)
      return r->failure;
  } else {
    size_t need = r->length + (size_t)ev->data_len + 1;
    if (need > r->capacity) {
      size_t cap = need + 1024;
      if (cap > r->limit + 1)
        cap = r->limit + 1;
      char *b = realloc(r->bytes, cap);
      if (!b) {
        r->failure = ESP_ERR_NO_MEM;
        return r->failure;
      }
      r->bytes = b;
      r->capacity = cap;
    }
    memcpy(r->bytes + r->length, ev->data, ev->data_len);
    r->bytes[r->length + ev->data_len] = 0;
  }
  r->length += ev->data_len;
  return ESP_OK;
}
static esp_err_t perform(const char *path, const char *method,
                         const cJSON *body, const char *interface,
                         response_t *result) {
  if (!path || path[0] != '/' || strstr(path, "..") || strchr(path, '\r') ||
      strchr(path, '\n') || strlen(path) > 1024 || !budget(result->deadline) ||
      !gw_core_time_trusted())
    return ESP_ERR_INVALID_STATE;
  char base[512], host[256], port[8] = "443", token[512], url[1600],
                             host_header[280];
  char *ca = calloc(1, 8192);
  if (!ca)
    return ESP_ERR_NO_MEM;
  esp_err_t e = gw_factory_get_string("api_url", base, sizeof(base));
  if (!strncmp(path, "/api/v1/gateway-ingress/batches", 30)) {
    char ingest[512];
    if (gw_factory_get_string("ingest_url", ingest, sizeof(ingest)) == ESP_OK)
      snprintf(base, sizeof(base), "%s", ingest);
  }
  if (e != ESP_OK || strncmp(base, "https://", 8) || strchr(base + 8, '@') ||
      strchr(base + 8, '?') || strchr(base + 8, '#')) {
    e = ESP_ERR_INVALID_ARG;
    goto done;
  }
  const char *end = strchr(base + 8, '/');
  size_t authority = end ? (size_t)(end - base - 8) : strlen(base + 8);
  if (end && strcmp(end, "/")) {
    e = ESP_ERR_INVALID_ARG;
    goto done;
  }
  if (!authority || authority >= sizeof(host)) {
    e = ESP_ERR_INVALID_ARG;
    goto done;
  }
  memcpy(host, base + 8, authority);
  host[authority] = 0;
  char *colon = strchr(host, ':');
  if (colon) {
    if (strlen(colon + 1) > 5 || !colon[1]) {
      e = ESP_ERR_INVALID_ARG;
      goto done;
    }
    strcpy(port, colon + 1);
    *colon = 0;
    for (char *p = port; *p; p++)
      if (!isdigit((unsigned char)*p)) {
        e = ESP_ERR_INVALID_ARG;
        goto done;
      }
    if (atoi(port) < 1 || atoi(port) > 65535) {
      e = ESP_ERR_INVALID_ARG;
      goto done;
    }
  }
  for (char *p = host; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.') {
      e = ESP_ERR_INVALID_ARG;
      goto done;
    }
  if (gw_factory_get_string("token", token, sizeof(token)) != ESP_OK ||
      !token[0] || strchr(token, '\r') || strchr(token, '\n') ||
      gw_factory_get_string("root_ca", ca, 8192) != ESP_OK) {
    e = ESP_ERR_INVALID_STATE;
    goto done;
  }
  esp_netif_t *netif = interface ? gw_core_uplink_netif(interface)
                                 : esp_netif_get_default_netif();
  struct ifreq iface = {0};
  struct in_addr address;
  if ((e = resolve(netif, host, &address, &iface, result->deadline)) != ESP_OK)
    goto done;
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &address, ip, sizeof(ip));
  snprintf(url, sizeof(url), "https://%s:%s%s", ip, port, path);
  snprintf(host_header, sizeof(host_header), "%s:%s", host, port);
  esp_http_client_config_t cfg = {.url = url,
                                  .cert_pem = ca,
                                  .common_name = host,
                                  .skip_cert_common_name_check = false,
                                  .disable_auto_redirect = true,
                                  .timeout_ms = budget(result->deadline),
                                  .max_authorization_retries = -1,
                                  .if_name = &iface,
                                  .event_handler = http_event,
                                  .user_data = result,
                                  .buffer_size = 2048,
                                  .buffer_size_tx = 2048};
  esp_http_client_handle_t h = esp_http_client_init(&cfg);
  if (!h) {
    e = ESP_ERR_NO_MEM;
    goto done;
  }
  char bearer[520];
  snprintf(bearer, sizeof(bearer), "Bearer %s", token);
  esp_http_client_set_header(h, "Authorization", bearer);
  esp_http_client_set_header(h, "Host", host_header);
  esp_http_client_set_header(h, "Accept", "application/json");
  esp_http_client_set_header(h, "Cache-Control", "no-store");
  esp_http_client_set_method(h, !strcmp(method, "POST")  ? HTTP_METHOD_POST
                                : !strcmp(method, "PUT") ? HTTP_METHOD_PUT
                                                         : HTTP_METHOD_GET);
  char *raw = body ? cJSON_PrintUnformatted(body) : NULL;
  if (body && !raw) {
    e = ESP_ERR_NO_MEM;
  } else {
    if (raw) {
      esp_http_client_set_header(h, "Content-Type", "application/json");
      esp_http_client_set_post_field(h, raw, strlen(raw));
    }
    e = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    if (e == ESP_OK && (status < 200 || status >= 300))
      e = ESP_FAIL;
    if (result->failure != ESP_OK)
      e = result->failure;
    if (!budget(result->deadline))
      e = ESP_ERR_TIMEOUT;
  }
  if (raw) {
    memset(raw, 0, strlen(raw));
    free(raw);
  }
  esp_http_client_cleanup(h);
  memset(bearer, 0, sizeof(bearer));
done:
  memset(token, 0, sizeof(token));
  free(ca);
  return e;
}
esp_err_t gw_http_json(const char *path, const char *method, const cJSON *body,
                       const char *interface, uint64_t deadline, cJSON **out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  response_t r = {.limit = JSON_LIMIT, .deadline = deadline};
  esp_err_t e = perform(path, method, body, interface, &r);
  if (e == ESP_OK) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < r.length; i++) {
      char c = r.bytes[i];
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
        if (++depth > 16) {
          e = ESP_ERR_INVALID_SIZE;
          break;
        }
      } else if (c == '}' || c == ']') {
        if (!depth) {
          e = ESP_ERR_INVALID_RESPONSE;
          break;
        }
        --depth;
      }
    }
    if (quoted || depth)
      e = ESP_ERR_INVALID_RESPONSE;
    if (e == ESP_OK) {
      const char *end = NULL;
      *out = r.bytes
                 ? cJSON_ParseWithLengthOpts(r.bytes, r.length + 1, &end, true)
                 : cJSON_CreateObject();
      if (!*out)
        e = ESP_ERR_INVALID_RESPONSE;
    }
  }
  if (r.bytes) {
    memset(r.bytes, 0, r.capacity);
    free(r.bytes);
  }
  return e;
}
esp_err_t gw_http_stream(const char *path, uint64_t deadline, size_t limit,
                         gw_http_sink_fn sink, void *ctx, size_t *received) {
  if (!sink || !received)
    return ESP_ERR_INVALID_ARG;
  response_t r = {
      .limit = limit, .deadline = deadline, .sink = sink, .context = ctx};
  esp_err_t e = perform(path, "GET", NULL, NULL, &r);
  *received = r.length;
  return e;
}
