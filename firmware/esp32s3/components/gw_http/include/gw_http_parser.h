#pragma once
/* Bounded HTTP/1.0/1.1 response framing. TLS ownership/deadlines stay with the
 * caller; read/sink errors propagate without further reads or sink calls. */
#include <stddef.h>
#include <stdint.h>
enum {
  GW_HTTP_BAD_RESPONSE = -10001,
  GW_HTTP_TOO_LARGE = -10002,
  GW_HTTP_BAD_STATUS = -10003,
  GW_HTTP_NO_MEMORY = -10004
};
typedef int (*gw_http_read_fn)(void *, uint8_t *, size_t, size_t *);
typedef int (*gw_http_consume_fn)(void *, const uint8_t *, size_t);
int gw_http_read_response(gw_http_read_fn, void *reader_context,
                          gw_http_consume_fn, void *consumer_context,
                          size_t body_limit);
