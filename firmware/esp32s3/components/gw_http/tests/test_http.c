/* Production HTTPS adapter; replace only SDK network/TLS/clock APIs. */
#include "../gw_http.c"
#include <assert.h>
enum {
  SUCCESS,
  CONTINUOUS,
  OVERSIZE,
  SINK_FAIL,
  READ_WAIT,
  CONNECT_WAIT,
  WRITE_WAIT,
  REDIRECT,
  TCP_WAIT
};
static unsigned mode, read_calls, write_calls, connect_calls, destroy_calls,
    sink_calls;
static uint64_t now;
static size_t response_position, request_length;
static char request[8192], interface_name[16];
static uint8_t dns_query[272];
static size_t dns_length;
static esp_netif_t candidate = {3}, default_netif = {7};
static esp_tls_t client;
static const char *success =
    "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n{\"x\":1}";
uint64_t gw_core_mono_ms(void) { return now; }
bool gw_core_time_trusted(void) { return true; }
void vTaskDelay(TickType_t ticks) { now += ticks; }
uint32_t esp_random(void) { return 123; }
esp_netif_t *gw_core_uplink_netif(const char *name) {
  assert(!strcmp(name, "ETHERNET"));
  return &candidate;
}
esp_netif_t *esp_netif_get_default_netif(void) { return &default_netif; }
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t *n, char *out) {
  snprintf(interface_name, sizeof(interface_name), "net%u", n->id);
  strcpy(out, interface_name);
  return ESP_OK;
}
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip) {
  ip->ip.addr = n->id;
  return ESP_OK;
}
esp_err_t esp_netif_get_dns_info(esp_netif_t *n, int kind,
                                 esp_netif_dns_info_t *dns) {
  (void)kind;
  dns->ip.type = ESP_IPADDR_TYPE_V4;
  dns->ip.u_addr.ip4.addr = 100 + n->id;
  return ESP_OK;
}
esp_err_t gw_factory_get_string(const char *name, char *out, size_t n) {
  const char *s = !strcmp(name, "api_url")   ? "https://api.example.test"
                  : !strcmp(name, "root_ca") ? "TEST_TRUSTED_CA"
                  : !strcmp(name, "token")   ? "TEST_DEVICE_CREDENTIAL"
                                             : NULL;
  if (!s)
    return ESP_ERR_NOT_FOUND;
  assert(strlen(s) < n);
  strcpy(out, s);
  return ESP_OK;
}
int inet_aton(const char *text, struct in_addr *out) {
  (void)text;
  (void)out;
  return 0;
}
const char *inet_ntop(int af, const void *addr, char *out, size_t n) {
  (void)addr;
  assert(af == AF_INET && n >= 12);
  strcpy(out, "192.0.2.123");
  return out;
}
uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
int socket(int af, int type, int protocol) {
  assert(af == AF_INET && type == SOCK_DGRAM && protocol == IPPROTO_UDP);
  return 99;
}
int setsockopt(int fd, int level, int option, const void *value, size_t n) {
  assert(fd == 99 && level == SOL_SOCKET);
  if (option == SO_BINDTODEVICE) {
    assert(n == sizeof(struct ifreq));
    assert(!strcmp(((const struct ifreq *)value)->ifr_name, interface_name));
  }
  return 0;
}
int bind(int fd, const void *value, size_t n) {
  (void)value;
  (void)n;
  assert(fd == 99);
  return 0;
}
int connect(int fd, const void *value, size_t n) {
  (void)value;
  (void)n;
  assert(fd == 99);
  return 0;
}
int send(int fd, const void *value, size_t n, int flags) {
  assert(fd == 99 && !flags && n <= sizeof(dns_query));
  memcpy(dns_query, value, n);
  dns_length = n;
  return (int)n;
}
int recv(int fd, void *value, size_t capacity, int flags) {
  assert(fd == 99 && !flags && capacity >= dns_length + 16);
  uint8_t *out = value;
  memcpy(out, dns_query, dns_length);
  out[2] = 0x81;
  out[3] = 0x80;
  out[7] = 1;
  uint8_t answer[16] = {0xc0, 12, 0, 1, 0,   1, 0, 0,
                        0,    10, 0, 4, 192, 0, 2, 123};
  memcpy(out + dns_length, answer, 16);
  return (int)dns_length + 16;
}
int close(int fd) {
  assert(fd == 99);
  return 0;
}
esp_tls_t *esp_tls_init(void) {
  assert(!client.alive);
  client.alive = true;
  return &client;
}
int esp_tls_conn_new_async(const char *host, int length, int port,
                           const esp_tls_cfg_t *cfg, esp_tls_t *tls) {
  assert(tls == &client && tls->alive && !strcmp(host, "192.0.2.123") &&
         length == (int)strlen(host) && port == 443);
  assert(cfg->non_block && !cfg->skip_common_name &&
         !strcmp(cfg->common_name, "api.example.test") &&
         !strcmp((const char *)cfg->cacert_buf, "TEST_TRUSTED_CA") &&
         cfg->cacert_bytes == 16);
  assert(cfg->if_name && !strcmp(cfg->if_name->ifr_name, interface_name) &&
         cfg->timeout_ms > 0 && cfg->timeout_ms <= 10000);
  connect_calls++;
  if (mode == TCP_WAIT) {
    now += (unsigned)cfg->timeout_ms;
    return 0;
  }
  now++;
  return mode == CONNECT_WAIT ? 0 : 1;
}
int esp_tls_get_conn_state(esp_tls_t *tls, esp_tls_conn_state_t *state) {
  assert(tls->alive);
  *state = mode == TCP_WAIT ? ESP_TLS_CONNECTING : ESP_TLS_HANDSHAKE;
  return ESP_OK;
}
ssize_t esp_tls_conn_write(esp_tls_t *tls, const void *bytes, size_t n) {
  assert(tls->alive);
  write_calls++;
  now++;
  if (mode == WRITE_WAIT)
    return ESP_TLS_ERR_SSL_WANT_WRITE;
  size_t take = n > 17 ? 17 : n;
  assert(request_length + take < sizeof(request));
  memcpy(request + request_length, bytes, take);
  request_length += take;
  request[request_length] = 0;
  return (ssize_t)take;
}
ssize_t esp_tls_conn_read(esp_tls_t *tls, void *out, size_t capacity) {
  assert(tls->alive);
  read_calls++;
  now++;
  if (mode == READ_WAIT)
    return ESP_TLS_ERR_SSL_WANT_READ;
  const char *text =
      mode == OVERSIZE
          ? "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\nsecret"
      : mode == SINK_FAIL ? "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nabcdef"
      : mode == REDIRECT
          ? "HTTP/1.1 302 Found\r\nLocation: https://elsewhere\r\n\r\n"
      : mode == CONTINUOUS
          ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          : success;
  size_t length = strlen(text);
  if (response_position >= length && mode == CONTINUOUS) {
    assert(capacity >= 6);
    memcpy(out, "1\r\nx\r\n", 6);
    return 6;
  }
  if (response_position >= length)
    return 0;
  size_t n = length - response_position,
         fragment = mode == OVERSIZE || mode == CONTINUOUS ? 2048
                    : mode == SINK_FAIL                    ? 1
                                                           : 7;
  if (n > fragment)
    n = fragment;
  if (n > capacity)
    n = capacity;
  memcpy(out, text + response_position, n);
  response_position += n;
  return (ssize_t)n;
}
int esp_tls_conn_destroy(esp_tls_t *tls) {
  assert(tls == &client && tls->alive);
  tls->alive = false;
  destroy_calls++;
  return 0;
}
static esp_err_t receive(const uint8_t *data, size_t n, void *ctx) {
  (void)data;
  (void)n;
  (void)ctx;
  sink_calls++;
  return mode == SINK_FAIL ? ESP_FAIL : ESP_OK;
}
static void reset(unsigned next) {
  assert(!client.alive);
  mode = next;
  read_calls = write_calls = connect_calls = destroy_calls = sink_calls = 0;
  now = 0;
  response_position = request_length = 0;
  memset(request, 0, sizeof(request));
}
int main(void) {
  reset(SUCCESS);
  cJSON *out = NULL;
  assert(gw_http_json("/api/v1/test", "GET", NULL, "ETHERNET", 200, &out) ==
         ESP_OK);
  assert(cJSON_GetObjectItemCaseSensitive(out, "x")->valueint == 1 &&
         destroy_calls == 1);
  assert(strstr(request, "Host: api.example.test:443\r\n") &&
         strstr(request, "Authorization: Bearer TEST_DEVICE_CREDENTIAL\r\n") &&
         strstr(request, "Connection: close\r\n"));
  cJSON_Delete(out);
  size_t received = 0;
  reset(CONTINUOUS);
  assert(gw_http_stream("/api/v1/file", 100, 100000, receive, NULL,
                        &received) == ESP_ERR_TIMEOUT);
  assert(now <= 100 && destroy_calls == 1 && sink_calls > 0 &&
         read_calls < 100);
  reset(OVERSIZE);
  assert(gw_http_stream("/api/v1/file", 200, 16, receive, NULL, &received) ==
         ESP_ERR_INVALID_SIZE);
  assert(read_calls == 1 && sink_calls == 0 && destroy_calls == 1);
  reset(SINK_FAIL);
  assert(gw_http_stream("/api/v1/file", 200, 100, receive, NULL, &received) ==
         ESP_FAIL);
  assert(sink_calls == 1 && received == 0 &&
         response_position ==
             strlen("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\na") &&
         destroy_calls == 1);
  reset(READ_WAIT);
  assert(gw_http_stream("/api/v1/file", 100, 100, receive, NULL, &received) ==
             ESP_ERR_TIMEOUT &&
         now <= 100 && destroy_calls == 1);
  reset(CONNECT_WAIT);
  assert(gw_http_stream("/api/v1/file", 20, 100, receive, NULL, &received) ==
             ESP_ERR_TIMEOUT &&
         now <= 20 && destroy_calls == 1 && read_calls == 0 &&
         write_calls == 0);
  reset(WRITE_WAIT);
  assert(gw_http_stream("/api/v1/file", 20, 100, receive, NULL, &received) ==
             ESP_ERR_TIMEOUT &&
         now <= 20 && destroy_calls == 1 && read_calls == 0);
  reset(REDIRECT);
  assert(gw_http_stream("/api/v1/file", 200, 100, receive, NULL, &received) ==
             ESP_FAIL &&
         destroy_calls == 1 && connect_calls == 1 && sink_calls == 0);
  reset(TCP_WAIT);
  assert(gw_http_stream("/api/v1/file", 20000, 100, receive, NULL, &received) ==
         ESP_ERR_TIMEOUT);
  assert(now == 10000 && connect_calls == 1 && destroy_calls == 1 &&
         read_calls == 0);
  puts("http adapter: same-owner TLS lifetime, SNI/Host/CA/netif, continuous "
       "chunk deadline, length/sink abort, read/write/connect deadline, "
       "redirect rejection passed");
  return 0;
}
