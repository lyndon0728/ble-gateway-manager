#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_core.h"
#include "gw_http.h"
#include "gw_http_parser.h"
#include "gw_http_private.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  int fd;
  uint64_t deadline;
  char body[4097];
  size_t used;
} time_io_t;
static int wait_socket(time_io_t *io, bool write) {
  uint64_t now = gw_core_mono_ms();
  if (now >= io->deadline)
    return ESP_ERR_TIMEOUT;
  uint64_t left = io->deadline - now;
  struct timeval tv = {.tv_sec = left / 1000, .tv_usec = (left % 1000) * 1000};
  fd_set set;
  FD_ZERO(&set);
  FD_SET(io->fd, &set);
  int r =
      select(io->fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
  return r > 0 && gw_core_mono_ms() < io->deadline ? ESP_OK : ESP_ERR_TIMEOUT;
}
static int read_time(void *ctx, uint8_t *data, size_t cap, size_t *received) {
  time_io_t *io = ctx;
  *received = 0;
  int e = wait_socket(io, false);
  if (e)
    return e;
  int n = recv(io->fd, data, cap, 0);
  if (n < 0)
    return ESP_FAIL;
  *received = n;
  return ESP_OK;
}
static int collect_time(void *ctx, const uint8_t *data, size_t length) {
  time_io_t *io = ctx;
  if (length > 4096 - io->used)
    return GW_HTTP_TOO_LARGE;
  memcpy(io->body + io->used, data, length);
  io->used += length;
  io->body[io->used] = 0;
  return ESP_OK;
}
esp_err_t gw_http_time_challenge(const char *nonce, const char *boot,
                                 uint64_t deadline, cJSON **out) {
  if (!out)
    return ESP_ERR_INVALID_ARG;
  *out = NULL;
  if (!nonce || strlen(nonce) != 44 || !boot || strlen(boot) != 32)
    return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < 32; i++)
    if (!((boot[i] >= '0' && boot[i] <= '9') ||
          (boot[i] >= 'a' && boot[i] <= 'f')))
      return ESP_ERR_INVALID_ARG;
  for (size_t i = 0; i < 44; i++)
    if (!(isalnum((unsigned char)nonce[i]) || nonce[i] == '+' ||
          nonce[i] == '/' || (i == 43 && nonce[i] == '=')))
      return ESP_ERR_INVALID_ARG;
  char url[320], host[256];
  unsigned port = 80;
  if (gw_factory_get_string("time_url", url, sizeof(url)) != ESP_OK ||
      strncmp(url, "http://", 7))
    return ESP_ERR_INVALID_STATE;
  const char *start = url + 7, *end = strchr(start, '/');
  size_t n = end ? (size_t)(end - start) : strlen(start);
  if (!n || n >= sizeof(host) || (end && strcmp(end, "/")))
    return ESP_ERR_INVALID_ARG;
  memcpy(host, start, n);
  host[n] = 0;
  char *colon = strchr(host, ':');
  if (colon) {
    *colon++ = 0;
    char *tail;
    unsigned long value = strtoul(colon, &tail, 10);
    if (!*colon || *tail || value < 1 || value > 65535)
      return ESP_ERR_INVALID_ARG;
    port = value;
  }
  if (!*host)
    return ESP_ERR_INVALID_ARG;
  for (char *p = host; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-')
      return ESP_ERR_INVALID_ARG;
  struct ifreq iface = {0};
  struct in_addr address;
  esp_err_t e = gw_http_resolve(esp_netif_get_default_netif(), host, &address,
                                &iface, deadline);
  if (e != ESP_OK)
    return e;
  time_io_t *io = calloc(1, sizeof(*io));
  if (!io)
    return ESP_ERR_NO_MEM;
  io->deadline = deadline;
  io->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (io->fd < 0) {
    free(io);
    return ESP_FAIL;
  }
  struct sockaddr_in remote = {
      .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = address};
  if (setsockopt(io->fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) ||
      fcntl(io->fd, F_SETFL, O_NONBLOCK) < 0) {
    e = ESP_FAIL;
    goto done;
  }
  int rc = connect(io->fd, (void *)&remote, sizeof(remote));
  if (rc < 0 && errno != EINPROGRESS) {
    e = ESP_FAIL;
    goto done;
  }
  if ((e = wait_socket(io, true)) != ESP_OK)
    goto done;
  int socket_error = 0;
  socklen_t len = sizeof(socket_error);
  if (getsockopt(io->fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) ||
      socket_error) {
    e = ESP_FAIL;
    goto done;
  }
  char body[180], request[768];
  int body_n =
      snprintf(body, sizeof(body),
               "{\"schemaVersion\":\"1.0\",\"nonce\":\"%s\",\"bootId\":\"%s\"}",
               nonce, boot);
  int request_n =
      snprintf(request, sizeof(request),
               "POST /api/v1/gateway-time-challenges HTTP/1.1\r\nHost: "
               "%s:%u\r\nContent-Type: application/json\r\nAccept-Encoding: "
               "identity\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
               host, port, body_n, body);
  if (body_n < 0 || body_n >= (int)sizeof(body) || request_n < 0 ||
      request_n >= (int)sizeof(request)) {
    e = ESP_ERR_INVALID_SIZE;
    goto done;
  }
  for (size_t pos = 0; pos < (size_t)request_n;) {
    if ((e = wait_socket(io, true)) != ESP_OK)
      goto done;
    int wrote = send(io->fd, request + pos, request_n - pos, 0);
    if (wrote <= 0) {
      e = ESP_FAIL;
      goto done;
    }
    pos += wrote;
  }
  e = gw_http_read_response(read_time, io, collect_time, io, 4096);
  if (e == ESP_OK && gw_core_mono_ms() < deadline) {
    /* This unauthenticated endpoint returns a flat three-string object.
     * Bound nesting before invoking the recursive JSON parser. */
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < io->used; i++) {
      char c = io->body[i];
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
        if (++depth > 1) {
          e = ESP_ERR_INVALID_RESPONSE;
          goto done;
        }
      } else if (c == '}' || c == ']') {
        if (!depth) {
          e = ESP_ERR_INVALID_RESPONSE;
          goto done;
        }
        --depth;
      }
    }
    if (quoted || depth || memchr(io->body, 0, io->used)) {
      e = ESP_ERR_INVALID_RESPONSE;
      goto done;
    }
    const char *tail = NULL;
    *out = cJSON_ParseWithLengthOpts(io->body, io->used + 1, &tail, true);
    if (!*out)
      e = ESP_ERR_INVALID_RESPONSE;
  } else if (e == ESP_OK)
    e = ESP_ERR_TIMEOUT;
done:
  close(io->fd);
  free(io);
  return e;
}
