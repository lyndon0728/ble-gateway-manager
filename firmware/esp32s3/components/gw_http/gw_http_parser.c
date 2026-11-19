#include "gw_http_parser.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_LIMIT 8192u
#define LINE_LIMIT 1024u
typedef struct {
  gw_http_read_fn read;
  void *context;
  uint8_t bytes[2048];
  char line[LINE_LIMIT + 1];
  size_t position, length;
} reader_t;
static int fill(reader_t *r) {
  r->position = r->length = 0;
  int err = r->read(r->context, r->bytes, sizeof(r->bytes), &r->length);
  if (err)
    return err;
  return r->length <= sizeof(r->bytes) ? 0 : GW_HTTP_BAD_RESPONSE;
}
static int byte(reader_t *r, uint8_t *out) {
  if (r->position == r->length) {
    int err = fill(r);
    if (err)
      return err;
  }
  if (r->position == r->length)
    return GW_HTTP_BAD_RESPONSE;
  *out = r->bytes[r->position++];
  return 0;
}
static int line(reader_t *r, char *out, size_t capacity, size_t *budget) {
  size_t length = 0;
  for (;;) {
    uint8_t c;
    int err = byte(r, &c);
    if (err)
      return err;
    if (!*budget)
      return GW_HTTP_TOO_LARGE;
    --*budget;
    if (c == '\r') {
      err = byte(r, &c);
      if (err)
        return err;
      if (!*budget)
        return GW_HTTP_TOO_LARGE;
      --*budget;
      if (c != '\n')
        return GW_HTTP_BAD_RESPONSE;
      out[length] = 0;
      return 0;
    }
    if (c == '\n' || !c || c == 127 || (c < 32 && c != '\t'))
      return GW_HTTP_BAD_RESPONSE;
    if (length + 1 >= capacity)
      return GW_HTTP_TOO_LARGE;
    out[length++] = (char)c;
  }
}
static bool equal(const char *a, const char *b) {
  while (*a && *b) {
    unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'A' && x <= 'Z')
      x += 32;
    if (y >= 'A' && y <= 'Z')
      y += 32;
    if (x != y)
      return false;
  }
  return !*a && !*b;
}
static bool token(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c);
}
static int field(char *line, char **value) {
  char *colon = strchr(line, ':');
  if (!colon || colon == line)
    return GW_HTTP_BAD_RESPONSE;
  for (char *p = line; p < colon; p++)
    if (!token(*p))
      return GW_HTTP_BAD_RESPONSE;
  *colon = 0;
  char *start = colon + 1;
  while (*start == ' ' || *start == '\t')
    start++;
  char *end = start + strlen(start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    *--end = 0;
  *value = start;
  return 0;
}
static bool decimal(const char *s, size_t *out) {
  size_t n = 0;
  if (!*s)
    return false;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || n > (SIZE_MAX - (unsigned)(*s - '0')) / 10)
      return false;
    n = n * 10 + (unsigned)(*s - '0');
  }
  *out = n;
  return true;
}
static int exact(reader_t *r, size_t n, gw_http_consume_fn sink,
                 void *context) {
  while (n) {
    if (r->position == r->length) {
      int err = fill(r);
      if (err)
        return err;
      if (!r->length)
        return GW_HTTP_BAD_RESPONSE;
    }
    size_t take = r->length - r->position;
    if (take > n)
      take = n;
    if (sink) {
      int err = sink(context, r->bytes + r->position, take);
      if (err)
        return err;
    }
    r->position += take;
    n -= take;
  }
  return 0;
}
static int parse(reader_t *r, gw_http_consume_fn sink, void *context,
                 size_t limit) {
  char *text = r->line;
  size_t metadata = HEADER_LIMIT;
  int err = line(r, text, sizeof(r->line), &metadata);
  if (err)
    return err;
  if (strlen(text) < 12 ||
      (strncmp(text, "HTTP/1.1 ", 9) && strncmp(text, "HTTP/1.0 ", 9)) ||
      text[9] < '0' || text[9] > '9' || text[10] < '0' || text[10] > '9' ||
      text[11] < '0' || text[11] > '9' || (text[12] && text[12] != ' '))
    return GW_HTTP_BAD_RESPONSE;
  unsigned status = (unsigned)(text[9] - '0') * 100 +
                    (unsigned)(text[10] - '0') * 10 +
                    (unsigned)(text[11] - '0');
  if (status < 200 || status >= 300)
    return GW_HTTP_BAD_STATUS;
  bool has_length = false, chunked = false;
  size_t length = 0;
  for (;;) {
    err = line(r, text, sizeof(r->line), &metadata);
    if (err)
      return err;
    if (!text[0])
      break;
    char *value;
    err = field(text, &value);
    if (err)
      return err;
    if (equal(text, "Content-Length")) {
      if (has_length || !decimal(value, &length))
        return GW_HTTP_BAD_RESPONSE;
      has_length = true;
    } else if (equal(text, "Transfer-Encoding")) {
      if (chunked || !equal(value, "chunked"))
        return GW_HTTP_BAD_RESPONSE;
      chunked = true;
    } else if (equal(text, "Content-Encoding") && !equal(value, "identity"))
      return GW_HTTP_BAD_RESPONSE;
  }
  if (has_length && chunked)
    return GW_HTTP_BAD_RESPONSE;
  if (status == 204) {
    return chunked || (has_length && length) ? GW_HTTP_BAD_RESPONSE : 0;
  }
  if (has_length) {
    if (length > limit)
      return GW_HTTP_TOO_LARGE;
    return exact(r, length, sink, context);
  }
  if (chunked) {
    size_t total = 0;
    for (;;) {
      metadata = 256;
      err = line(r, text, sizeof(r->line), &metadata);
      if (err)
        return err;
      const char *p = text;
      size_t chunk = 0;
      unsigned digits = 0;
      while (*p && *p != ';') {
        unsigned char c = (unsigned char)*p++;
        unsigned v = c >= '0' && c <= '9'   ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                     : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                            : 16;
        if (v == 16 || chunk > (SIZE_MAX - v) / 16)
          return GW_HTTP_BAD_RESPONSE;
        chunk = chunk * 16 + v;
        digits++;
      }
      if (!digits)
        return GW_HTTP_BAD_RESPONSE;
      if (!chunk) {
        metadata = HEADER_LIMIT;
        for (;;) {
          err = line(r, text, sizeof(r->line), &metadata);
          if (err)
            return err;
          if (!text[0])
            return 0;
          char *value;
          err = field(text, &value);
          if (err)
            return err;
          if (equal(text, "Content-Length") || equal(text, "Transfer-Encoding"))
            return GW_HTTP_BAD_RESPONSE;
        }
      }
      if (chunk > limit - total)
        return GW_HTTP_TOO_LARGE;
      err = exact(r, chunk, sink, context);
      if (err)
        return err;
      total += chunk;
      uint8_t cr, lf;
      err = byte(r, &cr);
      if (err)
        return err;
      err = byte(r, &lf);
      if (err)
        return err;
      if (cr != '\r' || lf != '\n')
        return GW_HTTP_BAD_RESPONSE;
    }
  }
  /* Close-delimited responses are allowed because requests explicitly close
   * the connection. EOF is success only in this framing mode. */
  size_t total = 0;
  for (;;) {
    if (r->position == r->length) {
      err = fill(r);
      if (err)
        return err;
      if (!r->length)
        return 0;
    }
    size_t n = r->length - r->position;
    if (n > limit - total)
      return GW_HTTP_TOO_LARGE;
    err = sink(context, r->bytes + r->position, n);
    if (err)
      return err;
    r->position += n;
    total += n;
  }
}
int gw_http_read_response(gw_http_read_fn read, void *reader_context,
                          gw_http_consume_fn consume, void *consumer_context,
                          size_t limit) {
  if (!read || !consume)
    return GW_HTTP_BAD_RESPONSE;
  reader_t *r = calloc(1, sizeof(*r));
  if (!r)
    return GW_HTTP_NO_MEMORY;
  r->read = read;
  r->context = reader_context;
  int result = parse(r, consume, consumer_context, limit);
  /* Pending credentials may appear in the TLS read-ahead buffer. */
  volatile uint8_t *p = (volatile uint8_t *)r;
  for (size_t i = 0; i < sizeof(*r); i++)
    p[i] = 0;
  free(r);
  return result;
}
