#include "gw_core.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static struct gw_frame frame;
static struct gw_frame decoded;
static struct gw_queue queue;

static const uint8_t hello_wire[] = {
    0x00,0x05,0x47,0x57,0x01,0x01,0x01,0x09,0x08,0x07,0x06,0x05,
    0x04,0x03,0x02,0x01,0x01,0x01,0x01,0x05,0x4f,0x42,0xa1,0xe8,0x00
};

static const uint8_t ble_wire[] = {
    0x00,0x06,0x47,0x57,0x01,0x20,0x25,0x0a,0x08,0x07,0x06,0x05,
    0x04,0x03,0x02,0x01,0x01,0x01,0x01,0x02,0x01,0x02,0x01,0x01,
    0x01,0x01,0x01,0x01,0x01,0x03,0xe8,0x03,0x01,0x01,0x01,0x01,
    0x01,0x01,0x02,0x03,0x0a,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0xc4,
    0x80,0x01,0x03,0xff,0x03,0x08,0x02,0x01,0x06,0x7d,0xec,0xfd,
    0xe0,0x00
};

static uint32_t random_state = 0x28c6a142;

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void test_codec(void)
{
    uint8_t wire[GW_WIRE_MAX];
    struct gw_decoder decoder;
    size_t length;

    assert(gw_crc32c((const uint8_t *)"123456789", 9) == UINT32_C(0xe3069283));
    assert(gw_crc32c(NULL, 0) == 0);
    memset(&frame, 0, sizeof(frame));
    frame.type = 1;
    frame.link_id = UINT64_C(0x0102030405060708);
    length = gw_encode_frame(&frame, wire, sizeof(wire));
    assert(length == sizeof(hello_wire));
    assert(memcmp(wire, hello_wire, length) == 0);
    assert(gw_decode_frame(hello_wire + 1, sizeof(hello_wire) - 2, &decoded));
    assert(decoded.type == 1 && decoded.payload_len == 0 && decoded.msg_id == 0);
    assert(decoded.link_id == frame.link_id);
    assert(gw_decode_frame(ble_wire + 1, sizeof(ble_wire) - 2, &decoded));
    assert(decoded.type == 0x20 && decoded.msg_id == 1 && decoded.payload_len == 37);
    assert(gw_get_u16(decoded.payload) == 1 && gw_get_u64(decoded.payload + 2) == 1);
    assert(gw_get_u64(decoded.payload + 10) == 1000);
    assert(decoded.payload[34] == 2 && decoded.payload[35] == 1 && decoded.payload[36] == 6);
    length = gw_encode_frame(&decoded, wire, sizeof(wire));
    assert(length == sizeof(ble_wire) && memcmp(wire, ble_wire, length) == 0);
    wire[11] ^= 0x40;
    assert(!gw_decode_frame(wire + 1, length - 2, &decoded));
    assert(!gw_decode_frame((const uint8_t *)"\x04\x01", 2, &decoded));
    assert(!gw_decode_frame((const uint8_t *)"\x02\x00", 2, &decoded));
    assert(!gw_decode_frame(NULL, 0, &decoded));
    assert(gw_encode_frame(&frame, wire, 24) == 0);

    gw_decoder_init(&decoder);
    for (size_t i = 0; i < GW_ENCODED_MAX + 100; ++i) {
        assert(gw_decoder_feed(&decoder, 0xff, &decoded) == 0);
    }
    assert(gw_decoder_feed(&decoder, 0, &decoded) == -1);
    assert(gw_decoder_feed(&decoder, 0, &decoded) == 0);
    for (size_t i = 0; i < sizeof(hello_wire); ++i) {
        assert(gw_decoder_feed(&decoder, hello_wire[i], &decoded) ==
               (i == sizeof(hello_wire) - 1 ? 1 : 0));
    }
    assert(gw_decoder_feed(&decoder, 0x13, &decoded) == 0);
    assert(gw_decoder_feed(&decoder, 0, &decoded) == -1);

    for (unsigned int trial = 0; trial < 600; ++trial) {
        frame.type = (uint8_t)next_random();
        frame.link_id = ((uint64_t)next_random() << 32) | next_random();
        frame.msg_id = next_random();
        frame.payload_len = trial < 4 ? GW_PAYLOAD_MAX :
                            (uint16_t)(next_random() % (GW_PAYLOAD_MAX + 1));
        for (size_t i = 0; i < frame.payload_len; ++i) {
            frame.payload[i] = trial == 0 ? 0 : trial == 1 ? 0xff : (uint8_t)next_random();
        }
        length = gw_encode_frame(&frame, wire, sizeof(wire));
        assert(length >= 25 && length <= sizeof(wire));
        assert(wire[0] == 0 && wire[length - 1] == 0);
        for (size_t i = 1; i < length - 1; ++i) {
            assert(wire[i] != 0);
        }
        assert(gw_decode_frame(wire + 1, length - 2, &decoded));
        assert(decoded.type == frame.type && decoded.link_id == frame.link_id);
        assert(decoded.msg_id == frame.msg_id && decoded.payload_len == frame.payload_len);
        assert(memcmp(decoded.payload, frame.payload, frame.payload_len) == 0);
        gw_decoder_init(&decoder);
        for (size_t i = 0; i < length; ++i) {
            assert(gw_decoder_feed(&decoder, wire[i], &decoded) == (i == length - 1 ? 1 : 0));
        }
    }
    frame.payload_len = GW_PAYLOAD_MAX + 1;
    assert(gw_encode_frame(&frame, wire, sizeof(wire)) == 0);

    /* Bounded garbage exercises every COBS exit under sanitizer builds. */
    for (unsigned int trial = 0; trial < 2000; ++trial) {
        length = next_random() % sizeof(wire);
        for (size_t i = 0; i < length; ++i) {
            wire[i] = (uint8_t)next_random();
        }
        (void)gw_decode_frame(wire, length, &decoded);
    }
}

static void test_profiles(void)
{
    struct gw_profile profile;
    struct gw_profile parsed;
    struct gw_profile unchanged;
    uint8_t encoded[GW_PROFILE_MAX + 1];
    uint8_t reencoded[GW_PROFILE_MAX];
    size_t length;

    gw_profile_default(&profile);
    assert(profile.revision == 0 && profile.scan_flags == 0);
    assert(profile.interval_ticks == 160 && profile.window_ticks == 80);
    assert(profile.min_rssi == INT8_MIN);
    length = gw_profile_encode(&profile, false, encoded, sizeof(encoded));
    assert(length == 16);
    assert(gw_profile_parse(encoded, length, false, &parsed));
    assert(parsed.rule_count == 0 && parsed.interval_ticks == 160);
    profile.revision = UINT64_C(0x123456789abcdef0);
    profile.scan_flags = 3;
    profile.min_rssi = -65;
    profile.uuid_count = GW_UUIDS_MAX;
    profile.company_count = GW_UUIDS_MAX;
    profile.rule_count = GW_RULES_MAX;
    for (unsigned int i = 0; i < GW_UUIDS_MAX; ++i) {
        profile.uuids[i][15] = (uint8_t)i;
        profile.companies[i] = (uint16_t)(i * 500U);
    }
    for (unsigned int i = 0; i < GW_RULES_MAX; ++i) {
        struct gw_beacon_rule *rule = &profile.rules[i];
        rule->action = (uint8_t)(i % 2);
        rule->kind_mask = 15;
        rule->ad_type = 0xff;
        rule->offset = 230;
        rule->length = 24;
        memset(rule->value, (int)i, 24);
        memset(rule->mask, 0xff, 24);
    }
    length = gw_profile_encode(&profile, true, encoded, sizeof(encoded));
    assert(length == GW_PROFILE_MAX);
    assert(gw_profile_parse(encoded, length, true, &parsed));
    assert(parsed.revision == profile.revision && parsed.uuid_count == GW_UUIDS_MAX);
    assert(parsed.rule_count == GW_RULES_MAX && parsed.rules[7].offset == 230);
    assert(gw_profile_encode(&parsed, true, reencoded, sizeof(reencoded)) == length);
    assert(memcmp(reencoded, encoded, length) == 0);
    assert(gw_profile_encode(&profile, true, reencoded, length - 1) == 0);
    memset(&unchanged, 0x5a, sizeof(unchanged));
    for (size_t i = 0; i < length; ++i) {
        parsed = unchanged;
        assert(!gw_profile_parse(encoded, i, true, &parsed));
        assert(memcmp(&parsed, &unchanged, sizeof(parsed)) == 0);
    }
    encoded[length] = 0;
    assert(!gw_profile_parse(encoded, length + 1, true, &parsed));
    /* Reject duplicate/unsorted UUIDs instead of silently changing a revision. */
    memcpy(encoded + 2 + 15 + 16, encoded + 2 + 15, 16);
    assert(!gw_profile_parse(encoded, length, true, &parsed));
    memcpy(encoded, reencoded, length);
    encoded[2 + 8] = 0x80;
    assert(!gw_profile_parse(encoded, length, true, &parsed));
    memcpy(encoded, reencoded, length);
    encoded[length - 1] = 0;
    memset(encoded + length - 24, 0, 24);
    assert(!gw_profile_parse(encoded, length, true, &parsed));

    gw_profile_default(&profile);
    length = gw_profile_encode(&profile, false, encoded, sizeof(encoded));
    encoded[13] = 21;
    assert(!gw_profile_parse(encoded, length, false, &parsed));
    encoded[13] = 0x80;
    gw_put_u16(encoded + 11, 161);
    assert(!gw_profile_parse(encoded, length, false, &parsed));
    for (unsigned int trial = 0; trial < 2000; ++trial) {
        length = next_random() % sizeof(encoded);
        for (size_t i = 0; i < length; ++i) {
            encoded[i] = (uint8_t)next_random();
        }
        (void)gw_profile_parse(encoded, length, (trial & 1U) != 0, &parsed);
    }
}

static void test_filters(void)
{
    static const uint8_t heart_uuid[16] = {
        0x00,0x00,0x18,0x0d,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5f,0x9b,0x34,0xfb
    };
    static const uint8_t ad16[] = {3,0x03,0x0d,0x18};
    static const uint8_t ad32[] = {5,0x05,0x0d,0x18,0,0};
    static const uint8_t service16[] = {5,0x16,0x0d,0x18,0xaa,0xbb};
    static const uint8_t combined[] = {3,0x03,0x0d,0x18,5,0xff,0x4c,0,0x02,0x15};
    static const uint8_t malformed[] = {3,0x03,0x0d};
    static const uint8_t cross_fields[] = {2,0xff,0xaa,2,0xff,0xbb};
    struct gw_profile profile;
    uint8_t ad128[18] = {17,0x07};

    gw_profile_default(&profile);
    assert(!gw_filter_match(&profile, 0, -60, ad16, sizeof(ad16)));
    profile.scan_flags = 1;
    assert(gw_filter_match(&profile, 0, -60, malformed, sizeof(malformed)));
    assert(!gw_ad_valid(malformed, sizeof(malformed)));
    assert(gw_ad_valid((const uint8_t *)"\x00\x00", 2));
    assert(!gw_ad_valid((const uint8_t *)"\x00\x01", 2));
    assert(!gw_filter_match(&profile, 4, -60, ad16, sizeof(ad16)));
    profile.min_rssi = -70;
    assert(!gw_filter_match(&profile, 0, INT8_MIN, ad16, sizeof(ad16)));
    assert(!gw_filter_match(&profile, 0, -71, ad16, sizeof(ad16)));
    assert(gw_filter_match(&profile, 0, -70, ad16, sizeof(ad16)));
    profile.uuid_count = 1;
    memcpy(profile.uuids[0], heart_uuid, 16);
    for (size_t i = 0; i < 16; ++i) {
        ad128[i + 2] = heart_uuid[15 - i];
    }
    assert(gw_filter_match(&profile, 0, -60, ad16, sizeof(ad16)));
    assert(gw_filter_match(&profile, 0, -60, ad32, sizeof(ad32)));
    assert(gw_filter_match(&profile, 0, -60, ad128, sizeof(ad128)));
    assert(gw_filter_match(&profile, 0, -60, service16, sizeof(service16)));
    assert(!gw_filter_match(&profile, 0, -60, malformed, sizeof(malformed)));
    assert(gw_filter_match(&profile, 0, -60,
                           (const uint8_t *)"\x03\x03\x0d\x18\x04\xff", 6));
    assert(!gw_filter_match(&profile, 0, -60, (const uint8_t *)"\x04\x03\x0d\x18\x00", 5));
    profile.company_count = 1;
    profile.companies[0] = 0x004c;
    assert(!gw_filter_match(&profile, 0, -60, ad16, sizeof(ad16)));
    assert(gw_filter_match(&profile, 0, -60, combined, sizeof(combined)));
    profile.rule_count = 2;
    profile.rules[0] = (struct gw_beacon_rule){
        .action = 0, .kind_mask = 1, .ad_type = 0xff, .offset = 2, .length = 2,
        .value = {0x02,0x15}, .mask = {0xff,0xff}
    };
    profile.rules[1] = profile.rules[0];
    profile.rules[1].action = 1;
    assert(!gw_filter_match(&profile, 0, -60, combined, sizeof(combined)));
    profile.rules[0].action = 1;
    profile.rules[1].action = 0;
    assert(gw_filter_match(&profile, 0, -60, combined, sizeof(combined)));
    assert(!gw_filter_match(&profile, 1, -60, combined, sizeof(combined)));
    profile.rule_count = 1;
    profile.rules[0].action = 0;
    assert(gw_filter_match(&profile, 1, -60, combined, sizeof(combined)));
    profile.uuid_count = profile.company_count = 0;
    profile.rules[0].action = 1;
    profile.rules[0].offset = 0;
    profile.rules[0].value[0] = 0xaa;
    profile.rules[0].value[1] = 0xbb;
    assert(!gw_filter_match(&profile, 0, -60, cross_fields, sizeof(cross_fields)));
    profile.rules[0].length = 1;
    profile.rules[0].mask[0] = 0xf0;
    assert(gw_filter_match(&profile, 0, -60, cross_fields, sizeof(cross_fields)));
    for (unsigned int trial = 0; trial < 2000; ++trial) {
        uint8_t garbage[300];
        size_t length = next_random() % sizeof(garbage);

        for (size_t i = 0; i < length; ++i) {
            garbage[i] = (uint8_t)next_random();
        }
        (void)gw_filter_match(&profile, (uint8_t)(trial % 4), -60, garbage, length);
    }
}

static size_t make_record(uint8_t *record, uint8_t type, size_t data_length, uint64_t capture)
{
    memset(record, 0, 24 + data_length);
    gw_put_u64(record, capture);
    if (type == 0x20) {
        record[8] = data_length > 31 ? 2 : 0;
    } else {
        gw_put_u32(record + 8, 42);
        record[21] = 1;
    }
    gw_put_u16(record + 22, (uint16_t)data_length);
    for (size_t i = 0; i < data_length; ++i) {
        record[24 + i] = (uint8_t)(i + capture);
    }
    return 24 + data_length;
}

static void test_queue(void)
{
    uint8_t record[1674];
    uint8_t frozen[GW_BATCH_MAX];
    size_t length;
    uint16_t frozen_length;

    gw_queue_init(&queue);
    assert(sizeof(queue) <= 40U * 1024U);
    assert(!gw_queue_batch(&queue, 100, &frame));
    length = make_record(record, 0x20, 3, 100);
    assert(gw_queue_push(&queue, 0x20, record, length, 100));
    assert(!gw_queue_batch(&queue, 149, &frame));
    assert(gw_queue_batch(&queue, 150, &frame));
    assert(frame.type == 0x20 && frame.msg_id == 1 && frame.payload_len == 37);
    assert(gw_get_u16(frame.payload) == 1 && gw_get_u64(frame.payload + 2) == 1);
    assert(memcmp(frame.payload + 10, record, length) == 0);
    frozen_length = frame.payload_len;
    memcpy(frozen, frame.payload, frozen_length);
    assert(!gw_queue_ack(&queue, 2, 0));
    assert(gw_queue_ack(&queue, 1, 1));
    assert(queue.count == 1 && queue.used_bytes == length);
    assert(gw_queue_push(&queue, 0x20, record, length, 160));
    assert(gw_queue_batch(&queue, 200, &frame));
    assert(frame.msg_id == 1 && frame.payload_len == frozen_length);
    assert(memcmp(frame.payload, frozen, frozen_length) == 0);
    assert(gw_queue_ack(&queue, 1, 0));
    assert(queue.count == 1 && queue.used_bytes == length);
    assert(gw_queue_batch(&queue, 210, &frame));
    assert(frame.msg_id == 2 && gw_get_u64(frame.payload + 2) == 2);
    assert(gw_queue_ack(&queue, 2, 2));
    assert(queue.fault && !gw_queue_batch(&queue, 1000, &frame));
    assert(!gw_queue_ack(&queue, 2, 0));
    assert(queue.count == 1);

    gw_queue_init(&queue);
    assert(gw_queue_push(&queue, 0x20, record, length, 0));
    length = make_record(record, 0x21, 0, 1);
    assert(gw_queue_push(&queue, 0x21, record, length, 0));
    assert(gw_queue_batch(&queue, 0, &frame));
    assert(frame.type == 0x20 && gw_get_u16(frame.payload) == 1);
    assert(gw_queue_ack(&queue, frame.msg_id, 0));
    assert(gw_queue_batch(&queue, 50, &frame));
    assert(frame.type == 0x21 && gw_get_u64(frame.payload + 2) == 2);
    assert(gw_queue_ack(&queue, frame.msg_id, 0));
    assert(queue.count == 0 && queue.used_bytes == 0);

    gw_queue_init(&queue);
    length = make_record(record, 0x20, 0, 0);
    for (unsigned int i = 0; i < GW_QUEUE_RECORDS; ++i) {
        assert(gw_queue_push(&queue, 0x20, record, length, 0));
    }
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    assert(queue.next_sequence == GW_QUEUE_RECORDS + 1);
    for (unsigned int i = 0; i < 4; ++i) {
        assert(gw_queue_batch(&queue, 0, &frame));
        assert(gw_get_u16(frame.payload) == 32);
        assert(gw_get_u64(frame.payload + 2) == 1 + i * 32);
        assert(gw_queue_ack(&queue, frame.msg_id, 0));
    }
    assert(queue.count == 0 && queue.used_bytes == 0);

    gw_queue_init(&queue);
    length = make_record(record, 0x20, 1650, 0);
    for (unsigned int i = 0; i < GW_QUEUE_BYTES / 1674; ++i) {
        assert(gw_queue_push(&queue, 0x20, record, length, 0));
    }
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    assert(queue.used_bytes == (GW_QUEUE_BYTES / 1674) * 1674);
    assert(gw_queue_batch(&queue, 0, &frame));
    assert(frame.payload_len == 1684 && gw_get_u16(frame.payload) == 1);
    while (queue.count != 0) {
        assert(gw_queue_batch(&queue, 50, &frame));
        assert(gw_queue_ack(&queue, frame.msg_id, 0));
    }
    /* Exercise byte-arena wrap, descriptor wrap, and retained contents. */
    for (unsigned int i = 0; i < 500; ++i) {
        length = make_record(record, 0x21, 1 + i % 512, i);
        assert(gw_queue_push(&queue, 0x21, record, length, i));
        assert(gw_queue_batch(&queue, i + 50, &frame));
        assert(frame.payload_len == length + 10);
        assert(memcmp(frame.payload + 10, record, length) == 0);
        assert(gw_queue_ack(&queue, frame.msg_id, 0));
    }
    assert(queue.used_bytes == 0);

    /* Wrap while older records are live: free space is a circular FIFO arena. */
    gw_queue_init(&queue);
    for (unsigned int i = 0; i < 16; ++i) {
        length = make_record(record, 0x20, 1650, i);
        assert(gw_queue_push(&queue, 0x20, record, length, 0));
    }
    for (unsigned int i = 0; i < 200; ++i) {
        assert(gw_queue_batch(&queue, 50, &frame));
        length = make_record(record, 0x20, 1650, i);
        assert(gw_get_u64(frame.payload + 2) == i + 1);
        assert(memcmp(frame.payload + 10, record, length) == 0);
        assert(gw_queue_ack(&queue, frame.msg_id, 0));
        length = make_record(record, 0x20, 1650, i + 16);
        assert(gw_queue_push(&queue, 0x20, record, length, 0));
    }
    assert(queue.count == 16 && queue.used_bytes == 16 * 1674);

    gw_queue_init(&queue);
    length = make_record(record, 0x20, 3, 0);
    record[22] = 4;
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    assert(queue.next_sequence == 1);
    record[22] = 3;
    assert(!gw_queue_push(&queue, 0x99, record, length, 0));
    length = make_record(record, 0x20, 32, 0);
    record[8] = 0;
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    length = make_record(record, 0x21, 513, 0);
    assert(!gw_queue_push(&queue, 0x21, record, length, 0));
    length = make_record(record, 0x20, 3, 0);
    queue.next_sequence = UINT64_MAX;
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    queue.next_sequence = 1;
    queue.next_batch = UINT32_MAX;
    assert(gw_queue_push(&queue, 0x20, record, length, 0));
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
    assert(gw_queue_batch(&queue, 50, &frame));
    assert(frame.msg_id == UINT32_MAX);
    assert(gw_queue_ack(&queue, frame.msg_id, 0));
    assert(queue.count == 0 && queue.next_batch == 0);
    assert(!gw_queue_push(&queue, 0x20, record, length, 0));
}

int main(void)
{
    test_codec();
    test_profiles();
    test_filters();
    test_queue();
    puts("gw_core: all codec, profile, filter and FIFO tests passed");
    return 0;
}
