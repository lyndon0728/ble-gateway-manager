#include "gw_http_parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  const char *input;
  size_t length, position, fragment, read_calls, sink_calls, used;
  int fail_read, fail_sink;
  char output[128];
} fixture_t;
static int read_bytes(void *ctx, uint8_t *out, size_t capacity, size_t *n) {
  fixture_t *f = ctx;
  f->read_calls++;
  if (f->fail_read && (int)f->read_calls == f->fail_read)
    return 77;
  *n = f->length - f->position;
  if (*n > f->fragment)
    *n = f->fragment;
  if (*n > capacity)
    *n = capacity;
  memcpy(out, f->input + f->position, *n);
  f->position += *n;
  return 0;
}
static int sink(void *ctx, const uint8_t *bytes, size_t n) {
  fixture_t *f = ctx;
  f->sink_calls++;
  if (f->fail_sink && (int)f->sink_calls == f->fail_sink)
    return 88;
  assert(f->used + n < sizeof(f->output));
  memcpy(f->output + f->used, bytes, n);
  f->used += n;
  return 0;
}
static int run(fixture_t *f, const char *text, size_t limit) {
  f->input = text;
  f->length = strlen(text);
  return gw_http_read_response(read_bytes, f, sink, f, limit);
}
int main(void) {
  for (size_t fragment = 1; fragment < 32; fragment++) {
    fixture_t f = {.fragment = fragment};
    assert(run(&f, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 10) ==
           0);
    assert(f.used == 5 && !memcmp(f.output, "hello", 5));
    f = (fixture_t){.fragment = fragment};
    assert(run(&f,
               "HTTP/1.1 200 OK\r\nTransfer-Encoding: "
               "chunked\r\n\r\n2;ext=v\r\nhe\r\n3\r\nllo\r\n0\r\nTrace: "
               "ok\r\n\r\n",
               10) == 0);
    assert(f.used == 5 && !memcmp(f.output, "hello", 5));
  }
  const char *bad[] = {
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: "
      "chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\na",
      "HTTP/1.1 200 OK\n\n",
      "HTTP/1.1 204 OK\r\nContent-Length: 3\r\n\r\nabc"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
    fixture_t f = {.fragment = 32};
    assert(run(&f, bad[i], 100) == GW_HTTP_BAD_RESPONSE);
  }
  fixture_t f = {.fragment = 32};
  assert(run(&f, "HTTP/1.1 307 Redirect\r\nLocation: https://evil\r\n\r\n",
             100) == GW_HTTP_BAD_STATUS &&
         f.sink_calls == 0);
  f = (fixture_t){.fragment = 256};
  assert(run(&f, "HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\nsecret",
             16) == GW_HTTP_TOO_LARGE &&
         f.sink_calls == 0 && f.read_calls == 1);
  f = (fixture_t){.fragment = 1, .fail_sink = 1};
  assert(run(&f, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 10) ==
             88 &&
         f.sink_calls == 1 && f.position == f.length - 4);
  f = (fixture_t){.fragment = 1, .fail_read = 8};
  assert(run(&f, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", 10) ==
             77 &&
         f.read_calls == 8 && f.sink_calls == 0);
  f = (fixture_t){.fragment = 3};
  assert(run(&f, "HTTP/1.0 200 OK\r\n\r\nhello", 10) == 0 && f.used == 5);
  char *large = malloc(10000);
  strcpy(large, "HTTP/1.1 200 OK\r\nLong: ");
  memset(large + strlen(large), 'x', 2000);
  large[2023] = 0;
  f = (fixture_t){.fragment = 32};
  assert(run(&f, large, 10) == GW_HTTP_TOO_LARGE);
  free(large);
  puts("http parser: fragmented framing, strict lengths, chunked trailers, "
       "bounded headers, early EOF, redirects, sink/read stop passed");
  return 0;
}
