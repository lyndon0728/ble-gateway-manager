#include "gw_core.h"

#include <limits.h>
#include <string.h>

uint16_t gw_get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

uint32_t gw_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t gw_get_u64(const uint8_t *p)
{
    return (uint64_t)gw_get_u32(p) | ((uint64_t)gw_get_u32(p + 4) << 32);
}

void gw_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

void gw_put_u32(uint8_t *p, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

void gw_put_u64(uint8_t *p, uint64_t value)
{
    gw_put_u32(p, (uint32_t)value);
    gw_put_u32(p + 4, (uint32_t)(value >> 32));
}

uint32_t gw_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

size_t gw_encode_frame(const struct gw_frame *frame, uint8_t *wire, size_t capacity)
{
    uint8_t raw[GW_RAW_MAX];
    size_t raw_length;
    size_t written = 2;
    size_t code_offset = 1;
    uint8_t code = 1;

    if (frame == NULL || wire == NULL || frame->payload_len > GW_PAYLOAD_MAX) {
        return 0;
    }
    raw_length = 22U + frame->payload_len;
    if (capacity < raw_length + raw_length / 254U + 3U) {
        return 0;
    }
    raw[0] = 0x47;
    raw[1] = 0x57;
    raw[2] = 1;
    raw[3] = frame->type;
    gw_put_u16(raw + 4, frame->payload_len);
    gw_put_u64(raw + 6, frame->link_id);
    gw_put_u32(raw + 14, frame->msg_id);
    memcpy(raw + 18, frame->payload, frame->payload_len);
    gw_put_u32(raw + raw_length - 4, gw_crc32c(raw, raw_length - 4));
    wire[0] = 0;
    for (size_t i = 0; i < raw_length; ++i) {
        if (raw[i] == 0) {
            wire[code_offset] = code;
            code_offset = written++;
            code = 1;
        } else {
            wire[written++] = raw[i];
            ++code;
            if (code == 255) {
                wire[code_offset] = code;
                code_offset = written++;
                code = 1;
            }
        }
    }
    wire[code_offset] = code;
    wire[written++] = 0;
    return written;
}

bool gw_decode_frame(const uint8_t *encoded, size_t length, struct gw_frame *frame)
{
    uint8_t raw[GW_RAW_MAX];
    size_t pos = 0;
    size_t decoded = 0;

    if (encoded == NULL || frame == NULL || length == 0 || length > GW_ENCODED_MAX) {
        return false;
    }
    while (pos < length) {
        uint8_t code = encoded[pos++];
        size_t run = (size_t)code - 1U;

        if (code == 0 || run > length - pos || run > sizeof(raw) - decoded) {
            return false;
        }
        for (size_t i = 0; i < run; ++i) {
            if (encoded[pos + i] == 0) {
                return false;
            }
        }
        memcpy(raw + decoded, encoded + pos, run);
        decoded += run;
        pos += run;
        if (code < 255 && pos < length) {
            if (decoded == sizeof(raw)) {
                return false;
            }
            raw[decoded++] = 0;
        }
    }
    if (decoded < 22 || raw[0] != 0x47 || raw[1] != 0x57 || raw[2] != 1 ||
        gw_get_u16(raw + 4) != decoded - 22 ||
        gw_get_u32(raw + decoded - 4) != gw_crc32c(raw, decoded - 4)) {
        return false;
    }
    frame->type = raw[3];
    frame->payload_len = gw_get_u16(raw + 4);
    frame->link_id = gw_get_u64(raw + 6);
    frame->msg_id = gw_get_u32(raw + 14);
    memcpy(frame->payload, raw + 18, frame->payload_len);
    return true;
}

void gw_decoder_init(struct gw_decoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
}

int gw_decoder_feed(struct gw_decoder *decoder, uint8_t byte, struct gw_frame *frame)
{
    if (byte == 0) {
        int result = 0;

        if (decoder->discard) {
            result = -1;
        } else if (decoder->length != 0) {
            result = gw_decode_frame(decoder->encoded, decoder->length, frame) ? 1 : -1;
        }
        decoder->length = 0;
        decoder->discard = false;
        return result;
    }
    if (!decoder->discard) {
        if (decoder->length == sizeof(decoder->encoded)) {
            decoder->discard = true;
        } else {
            decoder->encoded[decoder->length++] = byte;
        }
    }
    return 0;
}

void gw_profile_default(struct gw_profile *profile)
{
    memset(profile, 0, sizeof(*profile));
    profile->interval_ticks = 160;
    profile->window_ticks = 80;
    profile->min_rssi = INT8_MIN;
}

static bool parse_scan(const uint8_t *data, size_t length, struct gw_profile *profile)
{
    size_t pos;

    if (length < 16) {
        return false;
    }
    profile->revision = gw_get_u64(data);
    profile->scan_flags = data[8];
    profile->interval_ticks = gw_get_u16(data + 9);
    profile->window_ticks = gw_get_u16(data + 11);
    profile->min_rssi = (int8_t)data[13];
    profile->uuid_count = data[14];
    if ((profile->scan_flags & ~3U) != 0 || profile->interval_ticks < 16 ||
        profile->interval_ticks > 16384 || profile->window_ticks < 16 ||
        profile->window_ticks > profile->interval_ticks || profile->min_rssi > 20 ||
        profile->uuid_count > GW_UUIDS_MAX) {
        return false;
    }
    pos = 15U + (size_t)profile->uuid_count * 16U;
    if (pos >= length) {
        return false;
    }
    memcpy(profile->uuids, data + 15, (size_t)profile->uuid_count * 16U);
    for (unsigned int i = 1; i < profile->uuid_count; ++i) {
        if (memcmp(profile->uuids[i - 1], profile->uuids[i], 16) >= 0) {
            return false;
        }
    }
    profile->company_count = data[pos++];
    if (profile->company_count > GW_UUIDS_MAX ||
        length - pos != (size_t)profile->company_count * 2U) {
        return false;
    }
    for (unsigned int i = 0; i < profile->company_count; ++i) {
        profile->companies[i] = gw_get_u16(data + pos);
        pos += 2;
        if (i != 0 && profile->companies[i - 1] >= profile->companies[i]) {
            return false;
        }
    }
    return true;
}

bool gw_profile_parse(const uint8_t *data, size_t length, bool extended,
                      struct gw_profile *profile)
{
    struct gw_profile candidate = {0};
    size_t pos = 0;
    size_t scan_length = length;

    if (data == NULL || profile == NULL || length > GW_PROFILE_MAX) {
        return false;
    }
    if (extended) {
        if (length < 19) {
            return false;
        }
        scan_length = gw_get_u16(data);
        pos = 2;
        if (scan_length > length - 3) {
            return false;
        }
    }
    if (!parse_scan(data + pos, scan_length, &candidate)) {
        return false;
    }
    pos += scan_length;
    if (extended) {
        candidate.rule_count = data[pos++];
        if (candidate.rule_count > GW_RULES_MAX) {
            return false;
        }
        for (unsigned int i = 0; i < candidate.rule_count; ++i) {
            struct gw_beacon_rule *rule = &candidate.rules[i];
            uint8_t mask_bits = 0;

            if (length - pos < 6) {
                return false;
            }
            rule->action = data[pos++];
            rule->kind_mask = data[pos++];
            rule->ad_type = data[pos++];
            rule->offset = gw_get_u16(data + pos);
            pos += 2;
            rule->length = data[pos++];
            if (rule->action > 1 || rule->kind_mask == 0 || rule->kind_mask > 15 ||
                rule->ad_type == 0 || rule->length == 0 || rule->length > 24 ||
                (uint32_t)rule->offset + rule->length > 254 ||
                length - pos < (size_t)rule->length * 2U) {
                return false;
            }
            memcpy(rule->value, data + pos, rule->length);
            pos += rule->length;
            memcpy(rule->mask, data + pos, rule->length);
            pos += rule->length;
            for (unsigned int j = 0; j < rule->length; ++j) {
                mask_bits |= rule->mask[j];
            }
            if (mask_bits == 0) {
                return false;
            }
        }
    }
    if (pos != length) {
        return false;
    }
    *profile = candidate;
    return true;
}

size_t gw_profile_encode(const struct gw_profile *profile, bool extended,
                         uint8_t *data, size_t capacity)
{
    size_t scan_length;
    size_t needed;
    size_t pos = 0;

    if (profile == NULL || data == NULL || profile->uuid_count > GW_UUIDS_MAX ||
        profile->company_count > GW_UUIDS_MAX || profile->rule_count > GW_RULES_MAX) {
        return 0;
    }
    scan_length = 16U + (size_t)profile->uuid_count * 16U +
                  (size_t)profile->company_count * 2U;
    needed = scan_length;
    if (extended) {
        needed += 3;
        for (unsigned int i = 0; i < profile->rule_count; ++i) {
            if (profile->rules[i].length == 0 || profile->rules[i].length > 24) {
                return 0;
            }
            needed += 6U + (size_t)profile->rules[i].length * 2U;
        }
    }
    if (capacity < needed) {
        return 0;
    }
    if (extended) {
        gw_put_u16(data, (uint16_t)scan_length);
        pos = 2;
    }
    gw_put_u64(data + pos, profile->revision);
    data[pos + 8] = profile->scan_flags;
    gw_put_u16(data + pos + 9, profile->interval_ticks);
    gw_put_u16(data + pos + 11, profile->window_ticks);
    data[pos + 13] = (uint8_t)profile->min_rssi;
    data[pos + 14] = profile->uuid_count;
    pos += 15;
    memcpy(data + pos, profile->uuids, (size_t)profile->uuid_count * 16U);
    pos += (size_t)profile->uuid_count * 16U;
    data[pos++] = profile->company_count;
    for (unsigned int i = 0; i < profile->company_count; ++i) {
        gw_put_u16(data + pos, profile->companies[i]);
        pos += 2;
    }
    if (extended) {
        data[pos++] = profile->rule_count;
        for (unsigned int i = 0; i < profile->rule_count; ++i) {
            const struct gw_beacon_rule *rule = &profile->rules[i];

            data[pos++] = rule->action;
            data[pos++] = rule->kind_mask;
            data[pos++] = rule->ad_type;
            gw_put_u16(data + pos, rule->offset);
            pos += 2;
            data[pos++] = rule->length;
            memcpy(data + pos, rule->value, rule->length);
            pos += rule->length;
            memcpy(data + pos, rule->mask, rule->length);
            pos += rule->length;
        }
    }
    return pos;
}

bool gw_ad_valid(const uint8_t *ad, size_t length)
{
    size_t pos = 0;

    if (ad == NULL && length != 0) {
        return false;
    }
    while (pos < length) {
        uint8_t field_length = ad[pos++];

        if (field_length == 0) {
            while (pos < length) {
                if (ad[pos++] != 0) {
                    return false;
                }
            }
            return true;
        }
        if (field_length > length - pos) {
            return false;
        }
        pos += field_length;
    }
    return true;
}

static void normalize_uuid(const uint8_t *wire, size_t width, uint8_t uuid[16])
{
    static const uint8_t base[16] = {
        0, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb
    };
    size_t offset = width == 16 ? 0 : 4U - width;

    memcpy(uuid, base, 16);
    for (size_t i = 0; i < width; ++i) {
        uuid[offset + i] = wire[width - 1U - i];
    }
}

static bool uuid_field_matches(const struct gw_profile *profile, uint8_t type,
                               const uint8_t *data, size_t length)
{
    size_t width;
    bool service_data;

    switch (type) {
    case 0x02: case 0x03: case 0x16: width = 2; break;
    case 0x04: case 0x05: case 0x20: width = 4; break;
    case 0x06: case 0x07: case 0x21: width = 16; break;
    default: return false;
    }
    service_data = type == 0x16 || type == 0x20 || type == 0x21;
    if (length < width || (!service_data && length % width != 0)) {
        return false;
    }
    if (service_data) {
        length = width;
    }
    for (size_t pos = 0; pos < length; pos += width) {
        uint8_t uuid[16];

        normalize_uuid(data + pos, width, uuid);
        for (unsigned int i = 0; i < profile->uuid_count; ++i) {
            if (memcmp(profile->uuids[i], uuid, 16) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool rule_matches(const struct gw_beacon_rule *rule, uint8_t kind,
                         uint8_t type, const uint8_t *data, size_t length)
{
    if ((rule->kind_mask & (1U << kind)) == 0 || rule->ad_type != type ||
        (size_t)rule->offset + rule->length > length) {
        return false;
    }
    for (unsigned int i = 0; i < rule->length; ++i) {
        if ((data[rule->offset + i] & rule->mask[i]) != (rule->value[i] & rule->mask[i])) {
            return false;
        }
    }
    return true;
}

bool gw_filter_match(const struct gw_profile *profile, uint8_t kind, int8_t rssi,
                     const uint8_t *ad, size_t length)
{
    bool uuid_match;
    bool company_match;
    bool matched_rules[GW_RULES_MAX] = {false};
    bool has_allow = false;
    size_t pos = 0;

    if (profile == NULL || kind > 3 || (profile->scan_flags & 1U) == 0 ||
        (ad == NULL && length != 0)) {
        return false;
    }
    if (profile->min_rssi != INT8_MIN &&
        (rssi == INT8_MIN || rssi < profile->min_rssi)) {
        return false;
    }
    /* With no content filters, retain malformed AD for server diagnostics. */
    if (profile->uuid_count == 0 && profile->company_count == 0 && profile->rule_count == 0) {
        return true;
    }
    uuid_match = profile->uuid_count == 0;
    company_match = profile->company_count == 0;
    while (pos < length && ad[pos] != 0) {
        uint8_t field_length = ad[pos];
        uint8_t type;
        const uint8_t *data;
        size_t data_length;

        /* A malformed tail cannot be matched or joined to a previous field. */
        if (field_length > length - pos - 1U) {
            break;
        }
        type = ad[pos + 1];
        data = ad + pos + 2;
        data_length = (size_t)field_length - 1U;

        if (!uuid_match) {
            uuid_match = uuid_field_matches(profile, type, data, data_length);
        }
        if (!company_match && type == 0xff && data_length >= 2) {
            uint16_t company = gw_get_u16(data);

            for (unsigned int i = 0; i < profile->company_count; ++i) {
                company_match |= profile->companies[i] == company;
            }
        }
        for (unsigned int i = 0; i < profile->rule_count; ++i) {
            matched_rules[i] |= rule_matches(&profile->rules[i], kind, type, data, data_length);
        }
        pos += (size_t)field_length + 1U;
    }
    if (!uuid_match || !company_match) {
        return false;
    }
    for (unsigned int i = 0; i < profile->rule_count; ++i) {
        if (matched_rules[i]) {
            return profile->rules[i].action == 1;
        }
        has_allow |= profile->rules[i].action == 1;
    }
    return !has_allow;
}

void gw_queue_init(struct gw_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    queue->next_sequence = 1;
    queue->next_batch = 1;
}

static bool record_valid(uint8_t type, const uint8_t *record, size_t length)
{
    if (record == NULL || length < 24 || length > GW_BATCH_MAX - 10 ||
        gw_get_u16(record + 22) != length - 24) {
        return false;
    }
    if (type == 0x20) {
        return record[8] < 4 && length - 24 <= (record[8] < 2 ? 31U : 1650U);
    }
    if (type == 0x21) {
        return record[21] <= 1 && length - 24 <= 512;
    }
    if (type == 0x22) {
        return record[21] == 0 && gw_get_u16(record + 19) == 1 &&
               length > 24 && length - 24 <= 512;
    }
    return false;
}

bool gw_queue_push(struct gw_queue *queue, uint8_t type, const uint8_t *record,
                   size_t length, uint64_t now_ms)
{
    struct gw_queued_record *item;
    size_t first_length;

    if (!record_valid(type, record, length) || queue->count == GW_QUEUE_RECORDS ||
        length > GW_QUEUE_BYTES - queue->used_bytes ||
        queue->next_sequence == UINT64_MAX || queue->next_batch == 0 ||
        queue->next_batch > UINT32_MAX - queue->count) {
        return false;
    }
    item = &queue->records[(queue->head + queue->count) % GW_QUEUE_RECORDS];
    item->offset = queue->write_offset;
    item->length = (uint16_t)length;
    item->type = type;
    item->sequence = queue->next_sequence++;
    item->queued_ms = now_ms;
    first_length = GW_QUEUE_BYTES - queue->write_offset;
    if (first_length > length) {
        first_length = length;
    }
    memcpy(queue->bytes + queue->write_offset, record, first_length);
    memcpy(queue->bytes, record + first_length, length - first_length);
    queue->write_offset = (uint16_t)((queue->write_offset + length) % GW_QUEUE_BYTES);
    queue->used_bytes += (uint32_t)length;
    ++queue->count;
    return true;
}

static void copy_record(const struct gw_queue *queue, const struct gw_queued_record *record,
                        uint8_t *destination)
{
    size_t first_length = GW_QUEUE_BYTES - record->offset;

    if (first_length > record->length) {
        first_length = record->length;
    }
    memcpy(destination, queue->bytes + record->offset, first_length);
    memcpy(destination + first_length, queue->bytes, record->length - first_length);
}

bool gw_queue_batch(struct gw_queue *queue, uint64_t now_ms, struct gw_frame *frame)
{
    const struct gw_queued_record *first;
    uint16_t count = 0;
    size_t length = 10;

    if (queue->fault || queue->count == 0 || queue->next_batch == 0) {
        return false;
    }
    first = &queue->records[queue->head];
    if (queue->inflight_count != 0) {
        count = queue->inflight_count;
    } else {
        while (count < queue->count && count < 32) {
            const struct gw_queued_record *item =
                &queue->records[(queue->head + count) % GW_QUEUE_RECORDS];

            if (item->type != first->type || item->length > GW_BATCH_MAX - length) {
                break;
            }
            length += item->length;
            ++count;
        }
        if (count == queue->count && count < 32 &&
            (now_ms < first->queued_ms || now_ms - first->queued_ms < 50)) {
            return false;
        }
        queue->inflight_count = count;
        queue->inflight_batch = queue->next_batch;
    }
    frame->type = first->type;
    frame->msg_id = queue->inflight_batch;
    gw_put_u16(frame->payload, count);
    gw_put_u64(frame->payload + 2, first->sequence);
    length = 10;
    for (uint16_t i = 0; i < count; ++i) {
        const struct gw_queued_record *item =
            &queue->records[(queue->head + i) % GW_QUEUE_RECORDS];

        copy_record(queue, item, frame->payload + length);
        length += item->length;
    }
    frame->payload_len = (uint16_t)length;
    return true;
}

bool gw_queue_ack(struct gw_queue *queue, uint32_t batch, uint8_t status)
{
    if (queue->fault || queue->inflight_count == 0 ||
        batch != queue->inflight_batch || status > 2) {
        return false;
    }
    if (status == 2) {
        queue->fault = true;
    } else if (status == 0) {
        for (uint16_t i = 0; i < queue->inflight_count; ++i) {
            queue->used_bytes -= queue->records[queue->head].length;
            queue->head = (uint16_t)((queue->head + 1U) % GW_QUEUE_RECORDS);
        }
        queue->count -= queue->inflight_count;
        queue->inflight_count = 0;
        queue->inflight_batch = 0;
        ++queue->next_batch;
    }
    return true;
}
