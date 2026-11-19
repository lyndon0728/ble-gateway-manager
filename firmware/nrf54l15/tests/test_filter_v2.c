#include "gw_filter_v2.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char golden[] = "020113001000010000000000000003a00050008000000001f401013a0001011f0001000442636e2dfffffffffda50693a4e24fb1afcfc6eb07647825ffffffffffffffffffffffffffffffff00b5dd001200ff03000f00";
static const uint8_t adv[] = {2,1,6,26,255,0x4c,0,2,0x15,
  0xfd,0xa5,6,0x93,0xa4,0xe2,0x4f,0xb1,0xaf,0xcf,0xc6,0xeb,7,0x64,0x78,0x25,0x12,0x34,0,3,0xc5};
static const uint8_t rsp[] = {7,9,'B','c','n','-','0','1'};
static unsigned emitted;
static uint8_t output[2][55];
static size_t output_size[2];
static void collect(void *user, const uint8_t *p, size_t n) {
    (void)user; if (emitted < 2) { memcpy(output[emitted], p, n); output_size[emitted] = n; } ++emitted;
}
static unsigned hex(uint8_t ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; }
static size_t golden_bytes(uint8_t *out) {
    size_t n = strlen(golden)/2;
    for (size_t i = 0; i < n; ++i) out[i] = (hex(golden[2*i]) << 4) | hex(golden[2*i+1]);
    return n;
}
static void fixture(struct gw_filter_v2_config *c) {
    uint8_t bytes[GW_FILTER_V2_MAX]; size_t n = golden_bytes(bytes);
    assert(n == 87 && gw_filter_v2_parse(bytes, n, c));
}
static size_t record(uint8_t *out, uint64_t time, uint8_t kind, int8_t rssi,
                     const uint8_t *ad, size_t n) {
    memset(out, 0, 24); gw_put_u64(out, time); out[8] = kind; out[10] = 1;
    out[11] = 0xaa; out[17] = (uint8_t)rssi; gw_put_u16(out + 22, (uint16_t)n);
    if (n) memcpy(out + 24, ad, n); return n + 24;
}
static void feed(struct gw_filter_v2_state *s, const struct gw_filter_v2_config *c,
                 uint64_t t, uint8_t kind, int8_t rssi, const uint8_t *ad, size_t n) {
    uint8_t bytes[128]; size_t len = record(bytes,t,kind,rssi,ad,n);
    gw_filter_v2_feed(s,c,bytes,len,collect,NULL);
}
static bool pair(const struct gw_filter_v2_config *c, const uint8_t *a, size_t an,
                 const uint8_t *b, size_t bn, int8_t rssi) {
    struct gw_filter_v2_state s = {0}; emitted=0;
    feed(&s,c,1000,0,rssi,a,an); feed(&s,c,1100,1,-90,b,bn);
    assert(s.counters[GW_FILTER_SEEN] == 2); return emitted == 2;
}
static void report_config(struct gw_filter_v2_config *c, uint16_t fields) {
    fixture(c); c->scope=0; c->pair_window_ms=0; c->rules[0].field_mask=fields;
}
static bool report(const struct gw_filter_v2_config *c, int8_t rssi, const uint8_t *a, size_t n) {
    struct gw_filter_v2_state s={0}; emitted=0; feed(&s,c,1000,0,rssi,a,n); return emitted==1;
}
static void test_codec(void) {
    struct gw_filter_v2_config c, parsed, untouched; uint8_t bytes[GW_FILTER_V2_MAX+1],encoded[GW_FILTER_V2_MAX];
    size_t n=golden_bytes(bytes); fixture(&c);
    assert(c.legacy.revision==1 && c.engine==1 && c.scope==1 && c.pair_window_ms==500);
    assert(gw_filter_v2_encode(&c,encoded,sizeof(encoded))==n && !memcmp(bytes,encoded,n));
    memset(&untouched,0xa5,sizeof(untouched));
    for(size_t len=0;len<n;len++) { parsed=untouched; assert(!gw_filter_v2_parse(bytes,len,&parsed)); assert(!memcmp(&parsed,&untouched,sizeof(parsed))); }
    bytes[n]=0; assert(!gw_filter_v2_parse(bytes,n+1,&parsed));
    assert(!gw_filter_v2_parse(bytes,sizeof(bytes),&parsed));
    assert(!gw_filter_v2_encode(&c,encoded,n-1));
    c.rules[0].major_value=0x12ff;
    assert(gw_filter_v2_encode(&c,encoded,sizeof(encoded))==n && gw_filter_v2_parse(encoded,n,&parsed));
    assert(parsed.rules[0].major_value==0x12ff); /* ignored bits must round trip */
    c.rules[0].major_mask=0; assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded)));
    fixture(&c); c.rules[0].field_mask|=64; assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded)));
    fixture(&c); c.legacy.rule_count=1; assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded)));
    fixture(&c); c.legacy.scan_flags=1; assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded)));
    fixture(&c); c.legacy.uuid_count=2; assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded)));
    /* Largest LEGACY profile is preserved inside the wrapper. */
    gw_filter_v2_default(&c); c.legacy.uuid_count=c.legacy.company_count=128; c.legacy.rule_count=8;
    for(unsigned i=0;i<128;i++) { c.legacy.uuids[i][15]=(uint8_t)i; c.legacy.companies[i]=(uint16_t)i; }
    for(unsigned i=0;i<8;i++) { struct gw_beacon_rule *r=&c.legacy.rules[i]; r->action=1;r->kind_mask=15;r->ad_type=255;r->length=24; memset(r->mask,255,24); }
    assert(gw_filter_v2_encode(&c,encoded,sizeof(encoded))==2763);
    assert(gw_filter_v2_parse(encoded,2763,&parsed));
    c.engine=1;c.legacy.rule_count=0;c.rule_count=8;
    for(unsigned i=0;i<8;i++) { struct gw_filter_v2_rule *r=&c.rules[i];
        r->action=1;r->kind_mask=15;r->field_mask=63;r->name_length=29;
        memset(r->name_mask,255,29);memset(r->ibeacon_mask,255,16);memset(r->service_mask,255,16);
        r->rssi_arg0=(uint8_t)-127;r->rssi_arg1=20;r->major_mask=r->minor_mask=65535; }
    assert(!gw_filter_v2_encode(&c,encoded,sizeof(encoded))); /* 3467 > 3070 */
    /* 3070 is a real valid boundary: eight full rules, 104 UUIDs and 120 companies,
     * with one name shortened by one byte and one rule omitting three RSSI bytes. */
    c.legacy.uuid_count=104;c.legacy.company_count=120;c.rules[0].name_length=28;c.rules[1].field_mask=59;
    assert(gw_filter_v2_encode(&c,encoded,sizeof(encoded))==3062);
    c.legacy.company_count=124;
    assert(gw_filter_v2_encode(&c,encoded,sizeof(encoded))==3070);
    assert(gw_filter_v2_parse(encoded,3070,&parsed));
}
static void test_mask_fields(void) {
    struct gw_filter_v2_config c; uint8_t a[sizeof(adv)],b[sizeof(rsp)]; fixture(&c);
    assert(pair(&c,adv,sizeof(adv),rsp,sizeof(rsp),-60));
    assert(gw_get_u64(output[0])==1000 && gw_get_u64(output[1])==1100 && output[1][17]==(uint8_t)-90);
    assert(output_size[0]==sizeof(adv)+24 && !memcmp(output[0]+24,adv,sizeof(adv)));
    memcpy(a,adv,sizeof(a));a[26]=0xff;a[28]=0xa3;
    assert(pair(&c,a,sizeof(a),rsp,sizeof(rsp),-60)); a[25]=0x13;
    assert(!pair(&c,a,sizeof(a),rsp,sizeof(rsp),-60));a[25]=0x12;a[28]=4;
    assert(!pair(&c,a,sizeof(a),rsp,sizeof(rsp),-60));
    memset(c.rules[0].ibeacon_mask+4,0,12);memcpy(a,adv,sizeof(a));a[13]^=0xff;
    assert(pair(&c,a,sizeof(a),rsp,sizeof(rsp),-60));a[9]^=1;
    assert(!pair(&c,a,sizeof(a),rsp,sizeof(rsp),-60));
    fixture(&c); c.rules[0].name_mode=0;assert(!pair(&c,adv,sizeof(adv),rsp,sizeof(rsp),-60));
    fixture(&c);memcpy(b,rsp,sizeof(b));b[2]='b';assert(!pair(&c,adv,sizeof(adv),b,sizeof(b),-60));
    b[2]='B';b[1]=8;assert(!pair(&c,adv,sizeof(adv),b,sizeof(b),-60));
    c.rules[0].name_source=1;assert(pair(&c,adv,sizeof(adv),b,sizeof(b),-60));
    c.rules[0].name_source=2;assert(pair(&c,adv,sizeof(adv),b,sizeof(b),-60));
    fixture(&c);c.legacy.min_rssi=-50;assert(!pair(&c,adv,sizeof(adv),rsp,sizeof(rsp),-60));
}
static void test_rssi_and_order(void) {
    struct gw_filter_v2_config c; report_config(&c,4);
    const int8_t values[]={-76,-75,-35,-34,-128};
    for(unsigned i=0;i<5;i++)assert(report(&c,values[i],NULL,0)==(i==1||i==2));
    c.rules[0].rssi_mode=1;c.rules[0].rssi_arg0=(uint8_t)-64;c.rules[0].rssi_arg1=240;
    const int8_t bits[]={-65,-64,-49,-48,-128};
    for(unsigned i=0;i<5;i++)assert(report(&c,bits[i],NULL,0)==(i==1||i==2));
    c.rules[0].action=0;assert(!report(&c,-128,NULL,0));assert(report(&c,-65,NULL,0));
    report_config(&c,1);c.rule_count=2;c.rules[1]=c.rules[0];c.rules[1].field_mask=2;
    assert(report(&c,-60,adv,sizeof(adv)));c.rule_count=1;c.rules[0].field_mask=3;
    assert(!report(&c,-60,adv,sizeof(adv))); /* two OR rules are not field AND */
    c.rule_count=2;c.rules[0].field_mask=2;c.rules[0].action=0;c.rules[1]=c.rules[0];c.rules[1].action=1;
    assert(!report(&c,-60,adv,sizeof(adv)));c.rules[0].action=1;assert(report(&c,-60,adv,sizeof(adv)));
}
static void test_uuid_context(void) {
    struct gw_filter_v2_config c; report_config(&c,32);
    uint8_t uuid[]={0,0,0x18,0x0d,0,0,0x10,0,0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb};
    memcpy(c.rules[0].service_value,uuid,16);memset(c.rules[0].service_mask,255,16);
    uint8_t u16[]={3,3,0x0d,0x18},u32[]={5,5,0x0d,0x18,0,0},u128[18]={17,7};
    for(unsigned i=0;i<16;i++)u128[2+i]=uuid[15-i];
    assert(report(&c,-60,u16,sizeof(u16))&&report(&c,-60,u32,sizeof(u32))&&report(&c,-60,u128,sizeof(u128)));
    u16[1]=0x16;u32[1]=0x20;u128[1]=0x21;
    assert(report(&c,-60,u16,sizeof(u16))&&report(&c,-60,u32,sizeof(u32))&&report(&c,-60,u128,sizeof(u128)));
    assert(!report(&c,-60,adv,sizeof(adv)));
    /* Two iBeacon AD elements cannot donate UUID and major separately. */
    uint8_t two[2*sizeof(adv)];memcpy(two,adv,sizeof(adv));memcpy(two+sizeof(adv),adv,sizeof(adv));
    two[25]=0x13;two[sizeof(adv)+9]^=1;report_config(&c,10);assert(!report(&c,-60,two,sizeof(two)));
    uint8_t conflict[]={5,9,'B','c','n','-',5,9,'D','i','f','f'};
    struct gw_filter_v2_state s={0};report_config(&c,1);feed(&s,&c,1,0,-60,conflict,sizeof(conflict));
    assert(s.counters[GW_FILTER_UNSUPPORTED_CONTEXT]==1);
    uint8_t bad[]={0,1};assert(!report(&c,-60,bad,sizeof(bad)));
    gw_filter_v2_default(&c);c.legacy.scan_flags=1;assert(report(&c,-60,bad,sizeof(bad)));
}
static void test_pair_bounds(void) {
    struct gw_filter_v2_config c;struct gw_filter_v2_state s={0};uint8_t status[96],bytes[55]; fixture(&c);emitted=0;
    feed(&s,&c,1000,0,-60,adv,sizeof(adv));gw_filter_v2_advance(&s,1500);
    feed(&s,&c,1500,1,-128,rsp,sizeof(rsp));assert(emitted==2 && !s.counters[GW_FILTER_PAIR_TIMEOUT]);
    assert(output[1][17]==128); /* response unknown RSSI is preserved */
    memset(&s,0,sizeof(s));emitted=0;feed(&s,&c,1000,0,-60,adv,sizeof(adv));feed(&s,&c,1501,1,-60,rsp,sizeof(rsp));
    assert(!emitted&&s.counters[GW_FILTER_PAIR_TIMEOUT]==1&&s.counters[GW_FILTER_PAIR_AMBIGUOUS]==1);
    memset(&s,0,sizeof(s));feed(&s,&c,1000,0,-60,adv,sizeof(adv));feed(&s,&c,1100,0,-60,adv,sizeof(adv));feed(&s,&c,1200,1,-60,rsp,sizeof(rsp));
    assert(s.counters[GW_FILTER_PAIR_AMBIGUOUS]==3);
    memset(&s,0,sizeof(s));for(unsigned i=0;i<33;i++)feed(&s,&c,1000+i,0,-60,adv,sizeof(adv));
    gw_filter_v2_status(&s,&c,status);assert(gw_get_u16(status+10)==32 && gw_get_u32(status+12)==32*sizeof(adv));
    assert(s.counters[GW_FILTER_PAIR_OVERFLOW]==1);gw_filter_v2_clear(&s);
    assert(s.counters[GW_FILTER_CONFIG_DISCARDED]==32);gw_filter_v2_status(&s,&c,status);assert(!gw_get_u16(status+10));
    assert(gw_get_u64(status+16)==33 && gw_get_u64(status+88)==32);
    memset(&s,0,sizeof(s));feed(&s,&c,1000,0,-60,adv,sizeof(adv));size_t n=record(bytes,1100,1,-60,rsp,sizeof(rsp));bytes[11]++;
    gw_filter_v2_feed(&s,&c,bytes,n,collect,NULL);assert(s.counters[GW_FILTER_PAIR_AMBIGUOUS]==1);
    gw_filter_v2_clear(&s);feed(&s,&c,1200,1,-60,rsp,sizeof(rsp));assert(s.counters[GW_FILTER_PAIR_AMBIGUOUS]==2);
    memset(&s,0,sizeof(s));c.rules[0].action=0;feed(&s,&c,1000,0,-60,adv,sizeof(adv));gw_filter_v2_advance(&s,1501);
    assert(s.counters[GW_FILTER_PAIR_TIMEOUT]==1);feed(&s,&c,2000,0,-128,adv,sizeof(adv));gw_filter_v2_advance(&s,3000);
    assert(s.counters[GW_FILTER_UNKNOWN_RSSI]==1 && s.counters[GW_FILTER_PAIR_TIMEOUT]==1);
    n=record(bytes,4000,0,-60,adv,sizeof(adv));bytes[10]=255;gw_filter_v2_feed(&s,&c,bytes,n,collect,NULL);
    assert(s.counters[GW_FILTER_UNSUPPORTED_CONTEXT]==1);
    c.legacy.scan_flags=2;feed(&s,&c,4000,0,-60,adv,sizeof(adv));gw_filter_v2_status(&s,&c,status);assert(!gw_get_u16(status+10));
    /* ANY resolves once the complete pair exists: a complete name in the
     * response takes precedence over inconsistent shortened ADV names. */
    fixture(&c);c.rules[0].field_mask=1;c.rules[0].name_source=2;
    const uint8_t short_names[]={2,8,'x',2,8,'y'};
    assert(pair(&c,short_names,sizeof(short_names),rsp,sizeof(rsp),-60));
    c.rules[0].name_source=1;assert(!pair(&c,short_names,sizeof(short_names),rsp,sizeof(rsp),-60));
}
static void test_bounded_decode_fuzz(void) {
    struct gw_filter_v2_config c;uint8_t bytes[GW_FILTER_V2_MAX+1];uint32_t random=0x12345678;
    for(unsigned i=0;i<5000;i++) {
        size_t n=i%sizeof(bytes);
        for(size_t j=0;j<n;j++) {random=random*1664525U+1013904223U;bytes[j]=(uint8_t)(random>>24);}
        if(i&1) {size_t good=golden_bytes(bytes);n=good;bytes[i%good]^=(uint8_t)(i/2+1);}
        (void)gw_filter_v2_parse(bytes,n,&c);
    }
}
int main(void) {
    test_codec();test_mask_fields();test_rssi_and_order();test_uuid_context();test_pair_bounds();test_bounded_decode_fuzz();
    printf("PASS: Beacon V2 codec, masks, rule order, UUID normalization, bounded PAIR and counters (config=%zu state=%zu)\n",sizeof(struct gw_filter_v2_config),sizeof(struct gw_filter_v2_state));
    return 0;
}
