#include "gw_http.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_core.h"
#include "gw_http_parser.h"
#include "gw_http_private.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if !CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF
#error "Gateway candidate probes require per-interface DNS configuration"
#endif

#define JSON_LIMIT (128U * 1024U)
typedef struct {
  char *bytes;
  size_t length, capacity, limit;
  uint64_t deadline;
  gw_http_sink_fn sink;
  void *context;
  esp_err_t failure;
} response_t;
typedef struct {
  esp_tls_t *tls;
  uint64_t deadline;
} connection_t;
static void wipe(void *buffer, size_t n) {
  volatile unsigned char *p = buffer;
  while (n--)
    *p++ = 0;
}
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
esp_err_t gw_http_resolve(esp_netif_t *netif, const char *host,
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
static int consume(void *context, const uint8_t *bytes, size_t length) {
  response_t *r = context;
  if (r->failure != ESP_OK)
    return r->failure;
  if (!budget(r->deadline)) {
    r->failure = ESP_ERR_TIMEOUT;
    return r->failure;
  }
  if (length > r->limit - r->length) {
    r->failure = ESP_ERR_INVALID_SIZE;
    return r->failure;
  }
  if (r->sink) {
    r->failure = r->sink(bytes, length, r->context);
    if (r->failure)
      return r->failure;
  } else {
    size_t need = r->length + length + 1;
    if (need > r->capacity) {
      size_t cap = need + 1024;
      if (cap > r->limit + 1)
        cap = r->limit + 1;
      /* Avoid realloc leaving an old plaintext credential copy in the heap. */
      char *b = malloc(cap);
      if (!b) {
        r->failure = ESP_ERR_NO_MEM;
        return r->failure;
      }
      if (r->bytes) {
        memcpy(b, r->bytes, r->length);
        wipe(r->bytes, r->capacity);
        free(r->bytes);
      }
      r->bytes = b;
      r->capacity = cap;
    }
    memcpy(r->bytes + r->length, bytes, length);
    r->bytes[r->length + length] = 0;
  }
  r->length += length;
  return ESP_OK;
}
static int tls_read(void *context, uint8_t *bytes, size_t capacity,
                    size_t *received) {
  connection_t *c = context;
  *received = 0;
  while (budget(c->deadline)) {
    ssize_t n = esp_tls_conn_read(c->tls, bytes, capacity);
    if (!budget(c->deadline))
      return ESP_ERR_TIMEOUT;
    if (n >= 0) {
      *received = (size_t)n;
      return ESP_OK;
    }
    if (n != ESP_TLS_ERR_SSL_WANT_READ && n != ESP_TLS_ERR_SSL_WANT_WRITE)
      return ESP_FAIL;
    vTaskDelay(1);
  }
  return ESP_ERR_TIMEOUT;
}
static esp_err_t tls_write(connection_t *c, const void *buffer, size_t n) {
  const uint8_t *bytes = buffer;
  while (n) {
    if (!budget(c->deadline))
      return ESP_ERR_TIMEOUT;
    size_t take = n > 2048 ? 2048 : n;
    ssize_t written = esp_tls_conn_write(c->tls, bytes, take);
    if (!budget(c->deadline))
      return ESP_ERR_TIMEOUT;
    if (written > 0) {
      bytes += written;
      n -= (size_t)written;
      continue;
    }
    if (written != ESP_TLS_ERR_SSL_WANT_READ &&
        written != ESP_TLS_ERR_SSL_WANT_WRITE)
      return ESP_FAIL;
    vTaskDelay(1);
  }
  return ESP_OK;
}
static esp_err_t perform(const char *path, const char *method,
                         const cJSON *body, const char *interface,
                         response_t *result) {
  if (!method || !path || path[0] != '/' || strstr(path, "..") ||
      strchr(path, '\r') || strchr(path, '\n') || strlen(path) > 1024 ||
      !budget(result->deadline) || !gw_core_time_trusted())
    return ESP_ERR_INVALID_STATE;
  if (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "PUT"))
    return ESP_ERR_INVALID_ARG;
  esp_err_t prepared = gw_http_tls_prepare();
  if (prepared != ESP_OK)
    return prepared;
  for (const char *p = path; *p; p++)
    if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127)
      return ESP_ERR_INVALID_ARG;
  char base[512], host[256], port[8] = "443", token[512] = {0};
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
  if ((e = gw_http_resolve(netif, host, &address, &iface, result->deadline)) !=
      ESP_OK)
    goto done;
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &address, ip, sizeof(ip));
  esp_tls_cfg_t cfg = {.cacert_buf = (const unsigned char *)ca,
                       .crt_bundle_attach = gw_http_tls_attach,
                       .cacert_bytes = strlen(ca) + 1,
                       .common_name = host,
                       .skip_common_name = false,
                       .non_block = true,
                       .timeout_ms = budget(result->deadline),
                       .if_name = &iface};
  connection_t connection = {.tls = esp_tls_init(),
                             .deadline = result->deadline};
  if (!connection.tls) {
    e = ESP_ERR_NO_MEM;
    goto done;
  }
  for (;;) {
    int left = budget(result->deadline);
    if (!left) {
      e = ESP_ERR_TIMEOUT;
      break;
    }
    cfg.timeout_ms = left;
    int connected = esp_tls_conn_new_async(ip, (int)strlen(ip), atoi(port),
                                           &cfg, connection.tls);
    if (!budget(result->deadline)) {
      e = ESP_ERR_TIMEOUT;
      break;
    }
    if (connected == 1) {
      e = gw_http_tls_interval(connection.tls);
      break;
    }
    if (connected < 0) {
      e = ESP_FAIL;
      break;
    }
    /* SDK 5.5.6 retains select's emptied fd_set after a TCP timeout.
     * CONNECTING + 0 is an exhausted TCP wait, not a resumable handshake.
     * budget() bounds this wait by the deadline and by 10 seconds. */
    esp_tls_conn_state_t state;
    if (esp_tls_get_conn_state(connection.tls, &state) != ESP_OK) {
      e = ESP_FAIL;
      break;
    }
    if (state == ESP_TLS_CONNECTING) {
      e = ESP_ERR_TIMEOUT;
      break;
    }
    vTaskDelay(1);
  }
  char *raw = body ? cJSON_PrintUnformatted(body) : NULL;
  char *headers = malloc(4096);
  if (body && !raw) {
    e = ESP_ERR_NO_MEM;
  } else if (!headers)
    e = ESP_ERR_NO_MEM;
  else if (e == ESP_OK) {
    int n = snprintf(
        headers, 4096,
        "%s %s HTTP/1.1\r\nHost: %s:%s\r\nAuthorization: Bearer %s\r\nAccept: "
        "application/json\r\nAccept-Encoding: identity\r\nCache-Control: "
        "no-store\r\nConnection: close\r\nContent-Type: "
        "application/json\r\nContent-Length: %zu\r\n\r\n",
        method, path, host, port, token, raw ? strlen(raw) : 0);
    if (n < 0 || n >= 4096)
      e = ESP_ERR_INVALID_SIZE;
    else
      e = tls_write(&connection, headers, (size_t)n);
    if (e == ESP_OK && raw)
      e = tls_write(&connection, raw, strlen(raw));
    if (e == ESP_OK) {
      int parsed = gw_http_read_response(tls_read, &connection, consume, result,
                                         result->limit);
      e = parsed == GW_HTTP_BAD_RESPONSE ? ESP_ERR_INVALID_RESPONSE
          : parsed == GW_HTTP_TOO_LARGE  ? ESP_ERR_INVALID_SIZE
          : parsed == GW_HTTP_BAD_STATUS ? ESP_FAIL
          : parsed == GW_HTTP_NO_MEMORY  ? ESP_ERR_NO_MEM
                                         : parsed;
    }
  }
  /* A final sink (for example a Flash write) can cross the deadline after the
   * last TLS read; completion must also pass the absolute deadline check. */
  if (e == ESP_OK && (!budget(result->deadline) || !gw_core_time_trusted()))
    e = ESP_ERR_TIMEOUT;
  if (raw) {
    wipe(raw, strlen(raw));
    free(raw);
  }
  if (headers) {
    wipe(headers, 4096);
    free(headers);
  }
  /* The same task owns connect/read/write/abort/destruction. No timer can
   * race a client lifetime, and an error never resumes the transfer. */
  esp_tls_conn_destroy(connection.tls);
done:
  wipe(token, sizeof(token));
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
    wipe(r.bytes, r.capacity);
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
