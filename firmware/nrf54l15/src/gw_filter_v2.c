#include "gw_filter_v2.h"
#include <limits.h>
#include <string.h>

_Static_assert(sizeof(((struct gw_filter_v2_state *)0)->pending) <= 4096,
               "PAIR cache exceeds its total memory budget");

static bool nonzero(const uint8_t *p, size_t n) {
    uint8_t v = 0; for (size_t i = 0; i < n; ++i) v |= p[i]; return v != 0;
}
static bool masked(const uint8_t *p, const uint8_t *v, const uint8_t *m, size_t n) {
    for (size_t i = 0; i < n; ++i) if ((p[i] & m[i]) != (v[i] & m[i])) return false;
    return true;
}
static bool rssi_valid(int8_t v) { return v >= -127 && v <= 20; }
static size_t rule_size(const struct gw_filter_v2_rule *r, uint8_t scope) {
    if (r->action > 1 || !r->kind_mask || r->kind_mask > 15 ||
        (scope && r->kind_mask != 1) || !r->field_mask || r->field_mask > 63) return 0;
    size_t n = 4;
    if (r->field_mask & 1) {
        if (r->name_mode > 1 || r->name_source > 2 || !r->name_length ||
            r->name_length > 29 || !nonzero(r->name_mask, r->name_length)) return 0;
        n += 3 + 2 * r->name_length;
    }
    if (r->field_mask & 2) { if (!nonzero(r->ibeacon_mask, 16)) return 0; n += 32; }
    if (r->field_mask & 4) {
        if (r->rssi_mode > 1 || !rssi_valid((int8_t)r->rssi_arg0) ||
            (!r->rssi_mode && (!rssi_valid((int8_t)r->rssi_arg1) ||
             (int8_t)r->rssi_arg0 > (int8_t)r->rssi_arg1)) ||
            (r->rssi_mode && !r->rssi_arg1)) return 0;
        n += 3;
    }
    if (r->field_mask & 8) { if (!r->major_mask) return 0; n += 4; }
    if (r->field_mask & 16) { if (!r->minor_mask) return 0; n += 4; }
    if (r->field_mask & 32) { if (!nonzero(r->service_mask, 16)) return 0; n += 32; }
    return n;
}
void gw_filter_v2_default(struct gw_filter_v2_config *c) {
    memset(c, 0, sizeof(*c)); gw_profile_default(&c->legacy);
}
static bool config_valid(const struct gw_filter_v2_config *c) {
    if (!c || c->engine > 1 || c->scope > 1 || c->rule_count > 8 ||
        c->legacy.scan_flags > 3 || c->legacy.interval_ticks < 16 ||
        c->legacy.interval_ticks > 16384 || c->legacy.window_ticks < 16 ||
        c->legacy.window_ticks > c->legacy.interval_ticks ||
        (c->legacy.min_rssi != INT8_MIN && !rssi_valid(c->legacy.min_rssi)) ||
        c->legacy.uuid_count > GW_UUIDS_MAX || c->legacy.company_count > GW_UUIDS_MAX ||
        c->legacy.rule_count > GW_RULES_MAX) return false;
    for (unsigned i = 1; i < c->legacy.uuid_count; ++i)
        if (memcmp(c->legacy.uuids[i - 1], c->legacy.uuids[i], 16) >= 0) return false;
    for (unsigned i = 1; i < c->legacy.company_count; ++i)
        if (c->legacy.companies[i - 1] >= c->legacy.companies[i]) return false;
    for (unsigned i = 0; i < c->legacy.rule_count; ++i) {
        const struct gw_beacon_rule *r = &c->legacy.rules[i];
        if (r->action > 1 || !r->kind_mask || r->kind_mask > 15 || !r->ad_type ||
            !r->length || r->length > 24 || (uint32_t)r->offset + r->length > 254 ||
            !nonzero(r->mask, r->length)) return false;
    }
    if (!c->engine) return !c->scope && !c->pair_window_ms && !c->rule_count;
    if (c->legacy.rule_count || !c->rule_count ||
        (c->scope ? c->pair_window_ms != 500 || !(c->legacy.scan_flags & 2) :
                    c->pair_window_ms != 0)) return false;
    for (unsigned i = 0; i < c->rule_count; ++i) if (!rule_size(&c->rules[i], c->scope)) return false;
    return true;
}
bool gw_filter_v2_parse(const uint8_t *p, size_t n, struct gw_filter_v2_config *out) {
    if (!p || !out || n < 8 || n > GW_FILTER_V2_MAX || p[0] != 2) return false;
    struct gw_filter_v2_config c; memset(&c, 0, sizeof(c)); c.engine = p[1];
    size_t legacy = gw_get_u16(p + 2);
    if (legacy > n - 8 || !gw_profile_parse(p + 4, legacy, true, &c.legacy)) return false;
    size_t pos = 4 + legacy;
    c.scope = p[pos++]; c.pair_window_ms = gw_get_u16(p + pos); pos += 2;
    c.rule_count = p[pos++]; if (c.rule_count > 8) return false;
    for (unsigned i = 0; i < c.rule_count; ++i) {
        if (n - pos < 2) return false;
        size_t size = gw_get_u16(p + pos); pos += 2;
        if (size < 4 || size > n - pos) return false;
        size_t end = pos + size; struct gw_filter_v2_rule *r = &c.rules[i];
        r->action = p[pos++]; r->kind_mask = p[pos++]; r->field_mask = gw_get_u16(p + pos); pos += 2;
#define TAKE(dst, len) do { size_t count_ = (len); if (count_ > end - pos) return false; \
    memcpy((dst), p + pos, count_); pos += count_; } while (0)
        if (r->field_mask & 1) {
            if (end - pos < 3) return false;
            r->name_mode = p[pos++]; r->name_source = p[pos++]; r->name_length = p[pos++];
            if (r->name_length > 29) return false;
            TAKE(r->name_value, r->name_length); TAKE(r->name_mask, r->name_length);
        }
        if (r->field_mask & 2) { TAKE(r->ibeacon_value, 16); TAKE(r->ibeacon_mask, 16); }
        if (r->field_mask & 4) {
            if (end - pos < 3) return false;
            r->rssi_mode = p[pos++]; r->rssi_arg0 = p[pos++]; r->rssi_arg1 = p[pos++];
        }
        if (r->field_mask & 8) {
            if (end - pos < 4) return false;
            r->major_value = gw_get_u16(p + pos); r->major_mask = gw_get_u16(p + pos + 2); pos += 4;
        }
        if (r->field_mask & 16) {
            if (end - pos < 4) return false;
            r->minor_value = gw_get_u16(p + pos); r->minor_mask = gw_get_u16(p + pos + 2); pos += 4;
        }
        if (r->field_mask & 32) { TAKE(r->service_value, 16); TAKE(r->service_mask, 16); }
#undef TAKE
        if (pos != end) return false;
    }
    if (pos != n || !config_valid(&c)) return false;
    *out = c; return true;
}
size_t gw_filter_v2_encode(const struct gw_filter_v2_config *c, uint8_t *p, size_t cap) {
    if (!p || cap < 8 || !config_valid(c)) return 0;
    if (cap > GW_FILTER_V2_MAX) cap = GW_FILTER_V2_MAX;
    size_t legacy = gw_profile_encode(&c->legacy, true, p + 4, cap - 8);
    if (!legacy) return 0;
    size_t total = 8 + legacy;
    for (unsigned i = 0; i < c->rule_count; ++i) total += 2 + rule_size(&c->rules[i], c->scope);
    if (total > cap) return 0;
    p[0] = 2; p[1] = c->engine; gw_put_u16(p + 2, (uint16_t)legacy);
    size_t pos = 4 + legacy; p[pos++] = c->scope; gw_put_u16(p + pos, c->pair_window_ms); pos += 2;
    p[pos++] = c->rule_count;
    for (unsigned i = 0; i < c->rule_count; ++i) {
        const struct gw_filter_v2_rule *r = &c->rules[i];
        gw_put_u16(p + pos, (uint16_t)rule_size(r, c->scope)); pos += 2;
        p[pos++] = r->action; p[pos++] = r->kind_mask; gw_put_u16(p + pos, r->field_mask); pos += 2;
#define PUT(src, len) do { size_t count_ = (len); memcpy(p + pos, (src), count_); pos += count_; } while (0)
        if (r->field_mask & 1) {
            p[pos++] = r->name_mode; p[pos++] = r->name_source; p[pos++] = r->name_length;
            PUT(r->name_value, r->name_length); PUT(r->name_mask, r->name_length);
        }
        if (r->field_mask & 2) { PUT(r->ibeacon_value, 16); PUT(r->ibeacon_mask, 16); }
        if (r->field_mask & 4) { p[pos++] = r->rssi_mode; p[pos++] = r->rssi_arg0; p[pos++] = r->rssi_arg1; }
        if (r->field_mask & 8) { gw_put_u16(p + pos, r->major_value); gw_put_u16(p + pos + 2, r->major_mask); pos += 4; }
        if (r->field_mask & 16) { gw_put_u16(p + pos, r->minor_value); gw_put_u16(p + pos + 2, r->minor_mask); pos += 4; }
        if (r->field_mask & 32) { PUT(r->service_value, 16); PUT(r->service_mask, 16); }
#undef PUT
    }
    return pos;
}

struct ad_view { const uint8_t *p; size_t n; };
static bool strict_ad(struct ad_view a) {
    for (size_t p = 0; p < a.n;) {
        size_t n = a.p[p]; if (!n) return !nonzero(a.p + p, a.n - p);
        if (n > a.n - p - 1) return false;
        p += n + 1;
    }
    return true;
}
/* Return one logical name; contradictory names are never arbitrarily selected. */
static bool name_view(struct ad_view a, struct ad_view b, uint8_t type, struct ad_view *out) {
    *out = (struct ad_view){0};
    struct ad_view all[2] = {a, b};
    for (unsigned k = 0; k < 2; ++k) {
        for (size_t p = 0; p < all[k].n && all[k].p[p]; p += all[k].p[p] + 1) {
            if (all[k].p[p + 1] != type) continue;
            struct ad_view v = {all[k].p + p + 2, all[k].p[p] - 1U};
            if (out->p && (v.n != out->n || memcmp(v.p, out->p, v.n))) return false;
            *out = v;
        }
    }
    return true;
}
static bool names_valid(const struct gw_filter_v2_config *c, struct ad_view a, struct ad_view b,
                        bool complete_context) {
    for (unsigned i = 0; i < c->rule_count; ++i) {
        const struct gw_filter_v2_rule *r = &c->rules[i];
        if (!(r->field_mask & 1)) continue;
        struct ad_view name;
        if (!name_view(a, b, r->name_source == 1 ? 8 : 9, &name)) return false;
        if (complete_context && r->name_source == 2 && !name.p && !name_view(a, b, 8, &name)) return false;
    }
    return true;
}
static bool needs_rssi(const struct gw_filter_v2_config *c) {
    if (c->legacy.min_rssi != INT8_MIN) return true;
    for (unsigned i = 0; i < c->rule_count; ++i) if (c->rules[i].field_mask & 4) return true;
    return false;
}
static void normalize_uuid(const uint8_t *p, size_t n, uint8_t out[16]) {
    static const uint8_t base[16] = {0,0,0,0,0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};
    memcpy(out, base, 16); size_t off = n == 2 ? 2 : 0;
    for (size_t i = 0; i < n; ++i) out[off + i] = p[n - 1 - i];
}
static bool uuid_match(struct ad_view a, const uint8_t value[16], const uint8_t mask[16]) {
    for (size_t p = 0; p < a.n && a.p[p]; p += a.p[p] + 1) {
        uint8_t type = a.p[p + 1]; size_t n = a.p[p] - 1U, width = 0; bool single = false;
        if (type == 2 || type == 3 || type == 0x16) width = 2;
        if (type == 4 || type == 5 || type == 0x20) width = 4;
        if (type == 6 || type == 7 || type == 0x21) width = 16;
        if (!width) continue;
        single = type == 0x16 || type == 0x20 || type == 0x21;
        if (n < width || (!single && n % width)) continue;
        for (size_t q = 0; q + width <= n; q += width) {
            uint8_t u[16]; normalize_uuid(a.p + p + 2 + q, width, u);
            if (masked(u, value, mask, 16)) return true;
            if (single) break;
        }
    }
    return false;
}
static bool company_match(struct ad_view a, uint16_t id) {
    for (size_t p = 0; p < a.n && a.p[p]; p += a.p[p] + 1)
        if (a.p[p] >= 3 && a.p[p + 1] == 0xff && gw_get_u16(a.p + p + 2) == id) return true;
    return false;
}
static bool ibeacon_match(struct ad_view a, const struct gw_filter_v2_rule *r) {
    static const uint8_t head[] = {0x4c,0,2,0x15};
    for (size_t p = 0; p < a.n && a.p[p]; p += a.p[p] + 1) {
        if (a.p[p] != 26 || a.p[p + 1] != 0xff || memcmp(a.p + p + 2, head, 4)) continue;
        const uint8_t *v = a.p + p + 2;
        uint16_t major = ((uint16_t)v[20] << 8) | v[21], minor = ((uint16_t)v[22] << 8) | v[23];
        if ((r->field_mask & 2) && !masked(v + 4, r->ibeacon_value, r->ibeacon_mask, 16)) continue;
        if ((r->field_mask & 8) && (major & r->major_mask) != (r->major_value & r->major_mask)) continue;
        if ((r->field_mask & 16) && (minor & r->minor_mask) != (r->minor_value & r->minor_mask)) continue;
        return true;
    }
    return false;
}
static int context_match(const struct gw_filter_v2_config *c, const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn) {
    struct ad_view av = {a + 24, an - 24}, bv = {b ? b + 24 : NULL, b ? bn - 24 : 0};
    if (!strict_ad(av) || !strict_ad(bv)) return GW_FILTER_MALFORMED_AD;
    /* Validate all selected name sources before rule order can mask malformed context. */
    if (!names_valid(c, av, bv, true)) return GW_FILTER_UNSUPPORTED_CONTEXT;
    int8_t rssi = (int8_t)a[17];
    if (needs_rssi(c) && !rssi_valid(rssi)) return GW_FILTER_UNKNOWN_RSSI;
    if (!(c->legacy.scan_flags & 1) ||
        (c->legacy.min_rssi != INT8_MIN && rssi < c->legacy.min_rssi)) return GW_FILTER_RULE_DROPPED;
    bool uuid = !c->legacy.uuid_count, company = !c->legacy.company_count;
    static const uint8_t full[16] = {255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255};
    for (unsigned i = 0; !uuid && i < c->legacy.uuid_count; ++i)
        uuid = uuid_match(av, c->legacy.uuids[i], full) || uuid_match(bv, c->legacy.uuids[i], full);
    for (unsigned i = 0; !company && i < c->legacy.company_count; ++i)
        company = company_match(av, c->legacy.companies[i]) || company_match(bv, c->legacy.companies[i]);
    if (!uuid || !company) return GW_FILTER_RULE_DROPPED;
    bool has_allow = false;
    for (unsigned i = 0; i < c->rule_count; ++i) {
        const struct gw_filter_v2_rule *r = &c->rules[i]; has_allow |= r->action == 1;
        if (!(r->kind_mask & (1u << a[8]))) continue;
        if (r->field_mask & 1) {
            struct ad_view name;
            name_view(av, bv, r->name_source == 1 ? 8 : 9, &name);
            if (!name.p && r->name_source == 2) name_view(av, bv, 8, &name);
            if (!name.p || name.n < r->name_length || (!r->name_mode && name.n != r->name_length) ||
                !masked(name.p, r->name_value, r->name_mask, r->name_length)) continue;
        }
        if ((r->field_mask & 26) && !ibeacon_match(av, r) && !ibeacon_match(bv, r)) continue;
        if (r->field_mask & 4) {
            if ((!r->rssi_mode && (rssi < (int8_t)r->rssi_arg0 || rssi > (int8_t)r->rssi_arg1)) ||
                (r->rssi_mode && (((uint8_t)rssi & r->rssi_arg1) != (r->rssi_arg0 & r->rssi_arg1)))) continue;
        }
        if ((r->field_mask & 32) && !uuid_match(av, r->service_value, r->service_mask) &&
            !uuid_match(bv, r->service_value, r->service_mask)) continue;
        return r->action ? GW_FILTER_PASSED : GW_FILTER_RULE_DROPPED;
    }
    return has_allow ? GW_FILTER_RULE_DROPPED : GW_FILTER_PASSED;
}
void gw_filter_v2_advance(struct gw_filter_v2_state *s, uint64_t now) {
    for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) {
        struct gw_filter_v2_pending *p = &s->pending[i];
        uint64_t t = gw_get_u64(p->record);
        if (p->length && now > t && now - t > 500) {
            p->length = 0; ++s->counters[GW_FILTER_PAIR_TIMEOUT];
        }
    }
}
void gw_filter_v2_clear(struct gw_filter_v2_state *s) {
    for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) if (s->pending[i].length) {
        s->pending[i].length = 0; ++s->counters[GW_FILTER_CONFIG_DISCARDED];
    }
}
void gw_filter_v2_feed(struct gw_filter_v2_state *s, const struct gw_filter_v2_config *c,
                       const uint8_t *r, size_t n, gw_filter_v2_emit emit, void *user) {
    ++s->counters[GW_FILTER_SEEN];
    if (!r || n < 24 || n != 24U + gw_get_u16(r + 22) || r[8] > 3 || n > 24 + 1650U) {
        ++s->counters[GW_FILTER_UNSUPPORTED_CONTEXT]; return;
    }
    if (!c->engine) {
        bool pass = gw_filter_match(&c->legacy, r[8], (int8_t)r[17], r + 24, n - 24);
        ++s->counters[pass ? GW_FILTER_PASSED : GW_FILTER_RULE_DROPPED];
        if (pass && emit) emit(user, r, n);
        return;
    }
    if (!c->scope) {
        int reason = context_match(c, r, n, NULL, 0); ++s->counters[reason];
        if (reason == GW_FILTER_PASSED && emit) emit(user, r, n);
        return;
    }
    if (n > 55 || r[8] > 1 || r[10] > 1) { ++s->counters[GW_FILTER_UNSUPPORTED_CONTEXT]; return; }
    if (!strict_ad((struct ad_view){r + 24, n - 24})) { ++s->counters[GW_FILTER_MALFORMED_AD]; return; }
    if (!names_valid(c, (struct ad_view){r + 24, n - 24}, (struct ad_view){0}, false)) {
        ++s->counters[GW_FILTER_UNSUPPORTED_CONTEXT]; return;
    }
    /* Reject unusable anchor RSSI before cache/time faults can hide its cause.
     * The response RSSI is deliberately not used by a paired predicate. */
    if (!r[8] && needs_rssi(c) && !rssi_valid((int8_t)r[17])) {
        ++s->counters[GW_FILTER_UNKNOWN_RSSI]; return;
    }
    if (!(c->legacy.scan_flags & 1)) { ++s->counters[GW_FILTER_RULE_DROPPED]; return; }
    uint64_t now = gw_get_u64(r); gw_filter_v2_advance(s, now);
    if (!r[8]) {
        /* Capture address/identity/revision are scoped by the serialized owner. */
        for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) if (!s->pending[i].length) {
            memcpy(s->pending[i].record, r, n); s->pending[i].length = (uint8_t)n; return;
        }
        ++s->counters[GW_FILTER_PAIR_OVERFLOW]; return;
    }
    unsigned matches = 0, index = 0;
    for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) {
        struct gw_filter_v2_pending *p = &s->pending[i]; uint64_t t = gw_get_u64(p->record);
        if (p->length && now >= t && now - t <= 500 && !memcmp(p->record + 10, r + 10, 7)) {
            ++matches; index = i;
        }
    }
    if (matches != 1) {
        ++s->counters[GW_FILTER_PAIR_AMBIGUOUS];
        if (matches) for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) {
            struct gw_filter_v2_pending *p = &s->pending[i]; uint64_t t = gw_get_u64(p->record);
            if (p->length && now >= t && now - t <= 500 && !memcmp(p->record + 10, r + 10, 7)) {
                p->length = 0; ++s->counters[GW_FILTER_PAIR_AMBIGUOUS];
            }
        }
        return;
    }
    struct gw_filter_v2_pending *p = &s->pending[index];
    int reason = context_match(c, p->record, p->length, r, n); s->counters[reason] += 2;
    if (reason == GW_FILTER_PASSED && emit) { emit(user, p->record, p->length); emit(user, r, n); }
    p->length = 0;
}
void gw_filter_v2_status(const struct gw_filter_v2_state *s, const struct gw_filter_v2_config *c, uint8_t out[96]) {
    memset(out, 0, 96); gw_put_u64(out, c->legacy.revision); out[8] = c->engine; out[9] = c->scope;
    uint16_t count = 0; uint32_t bytes = 0;
    for (unsigned i = 0; i < GW_FILTER_V2_PENDING; ++i) if (s->pending[i].length) {
        ++count; bytes += s->pending[i].length - 24;
    }
    gw_put_u16(out + 10, count); gw_put_u32(out + 12, bytes);
    for (unsigned i = 0; i < GW_FILTER_COUNTER_COUNT; ++i) gw_put_u64(out + 16 + i * 8, s->counters[i]);
}
