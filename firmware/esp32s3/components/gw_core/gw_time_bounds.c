#include "gw_time_bounds.h"
#include <stddef.h>
bool gw_time_anchor_set(gw_time_anchor_t *a, uint64_t issued,
                        uint32_t uncertainty, uint64_t sent,
                        uint64_t received) {
  if (!a || received < sent || received - sent > 2000 || uncertainty > 1000 ||
      issued < 1577836800000ULL || issued > 4133980800000ULL)
    return false;
  uint64_t rtt = received - sent, q = 2 + (rtt + 9999) / 10000;
  *a = (gw_time_anchor_t){.lower = issued - uncertainty - q,
                          .upper = issued + uncertainty + rtt + q,
                          .received = received,
                          .expires = issued + 60000,
                          .valid = true};
  return true;
}
bool gw_time_anchor_bounds(const gw_time_anchor_t *a, uint64_t now,
                           uint64_t *lower, uint64_t *upper) {
  if (!a || !a->valid || now < a->received || now - a->received > 60000)
    return false;
  uint64_t age = now - a->received, drift = (age + 9999) / 10000;
  uint64_t lo = a->lower + age - drift, hi = a->upper + age + drift;
  if (hi >= a->expires)
    return false;
  if (lower)
    *lower = lo;
  if (upper)
    *upper = hi;
  return true;
}
