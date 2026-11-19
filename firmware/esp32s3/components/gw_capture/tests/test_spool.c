#include "gw_capture_rules.h"
#include "gw_spool.h"
#include "gw_wire.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t bytes[GW_SPOOL_BYTES];
  unsigned op, fail, mode;
} flash_t;
static bool fails(flash_t *f) { return ++f->op == f->fail; }
static int rd(void *ctx, uint32_t at, void *out, size_t n) {
  flash_t *f = ctx;
  assert(at + n <= sizeof(f->bytes));
  if (fails(f))
    return 1;
  memcpy(out, f->bytes + at, n);
  return 0;
}
static int wr(void *ctx, uint32_t at, const void *data, size_t n) {
  flash_t *f = ctx;
  assert(at + n <= sizeof(f->bytes) && !(at % 16) && !(n % 16));
  bool fail = fails(f);
  size_t actual = fail ? (f->mode == 0   ? 0
                          : f->mode == 1 ? (n / 2 / 16) * 16
                                         : n)
                       : n;
  for (size_t i = 0; i < actual; i++) {
    uint8_t v = ((const uint8_t *)data)[i];
    assert((f->bytes[at + i] & v) == v);
    f->bytes[at + i] = v;
  }
  return fail;
}
static int er(void *ctx, uint32_t at, size_t n) {
  flash_t *f = ctx;
  assert(!(at % 4096) && !(n % 4096));
  bool fail = fails(f);
  size_t actual = fail ? (f->mode == 0 ? 0 : f->mode == 1 ? n / 2 : n) : n;
  memset(f->bytes + at, 255, actual);
  return fail;
}
static int empty(void *ctx, uint32_t at, size_t n, bool *out) {
  flash_t *f = ctx;
  *out = true;
  for (size_t i = 0; i < n; i++)
    if (f->bytes[at + i] != 255)
      *out = false;
  return 0;
}
static gw_spool_io_t io(flash_t *f) {
  gw_spool_io_t v = {f, rd, wr, er, empty};
  return v;
}
static uint8_t boot[16] = {1}, session[16] = {2};
static void fresh(flash_t *f, gw_spool_t *s) {
  memset(f, 0, sizeof(*f));
  memset(f->bytes, 255, sizeof(f->bytes));
  gw_spool_io_t v = io(f);
  assert(gw_spool_open(s, &v, session) == GW_SP_OK);
}
static gw_spool_record_t record(unsigned radio, unsigned value) {
  gw_spool_record_t r = {.type = 0x20,
                         .radio = (uint8_t)radio,
                         .length = 27,
                         .arrived_ms = 1000,
                         .mapped_ms = 900,
                         .error_ms = 2,
                         .mapped = true};
  memcpy(r.source_boot, boot, 16);
  gw_put_u64(r.bytes, 900);
  r.bytes[9] = 1;
  r.bytes[10] = 0;
  memset(r.bytes + 11, 0xaa, 6);
  r.bytes[17] = (uint8_t)-60;
  r.bytes[18] = 128;
  r.bytes[19] = 1;
  r.bytes[21] = 255;
  gw_put_u16(r.bytes + 22, 3);
  r.bytes[24] = 2;
  r.bytes[25] = 1;
  r.bytes[26] = (uint8_t)value;
  return r;
}
static gw_spool_watermark_t watermark(uint32_t batch, uint64_t first,
                                      unsigned count) {
  gw_spool_watermark_t w = {
      .batch = batch, .first = first, .count = (uint16_t)count, .type = 0x20};
  memcpy(w.boot, boot, 16);
  memset(w.hash, (int)batch, 32);
  return w;
}
static unsigned count_state(gw_spool_t *s, unsigned state) {
  unsigned n = 0;
  for (unsigned i = 0; i < GW_SPOOL_SLOTS; i++)
    if (gw_spool_state(s, i) == state)
      n++;
  return n;
}
static void test_replay_and_capacity(void) {
  flash_t *f = malloc(sizeof(*f));
  gw_spool_t s;
  fresh(f, &s);
  gw_spool_record_t a[2] = {record(0, 6), record(0, 7)};
  gw_spool_watermark_t w = watermark(1, 1, 2);
  assert(gw_spool_add(&s, a, 2, &w) == GW_SP_OK);
  assert(gw_spool_check(&s, &w) == GW_SP_REPLAY);
  gw_spool_pair_t finish[2] = {{0, GW_SPOOL_NONE}, {1, GW_SPOOL_NONE}};
  assert(gw_spool_finish(&s, finish, 2) == GW_SP_OK);
  uint16_t slots[2] = {0, 1};
  uint8_t states[2] = {GW_SP_FREE, GW_SP_QUARANTINE};
  assert(gw_spool_mark(&s, slots, states, 2) == GW_SP_OK);
  gw_spool_close(&s);
  gw_spool_io_t v = io(f);
  assert(gw_spool_open(&s, &v, session) == GW_SP_OK);
  assert(gw_spool_check(&s, &w) == GW_SP_REPLAY);
  assert(count_state(&s, GW_SP_QUARANTINE) == 1);
  gw_spool_watermark_t next = watermark(2, 3, 1);
  assert(gw_spool_add(&s, a, 1, &next) == GW_SP_OK);
  assert(gw_spool_check(&s, &w) == GW_SP_OLD);
  for (unsigned i = 2; i < GW_SPOOL_SLOTS; i++)
    assert(gw_spool_add(&s, a, 1, NULL) == GW_SP_OK);
  assert(gw_spool_add(&s, a, 1, NULL) == GW_SP_FULL);
  assert(count_state(&s, GW_SP_QUARANTINE) == 1);
  gw_spool_close(&s);
  free(f);
}
static void test_faults_and_inbox(void) {
  flash_t *f = malloc(sizeof(*f));
  gw_spool_t s;
  fresh(f, &s);
  gw_spool_record_t a = record(0, 6);
  gw_spool_watermark_t w = watermark(1, 1, 1);
  assert(!gw_spool_add(&s, &a, 1, &w));
  w.hash[0] ^= 1;
  assert(gw_spool_check(&s, &w) == GW_SP_FAULT);
  gw_spool_close(&s);
  fresh(f, &s);
  a.type = 0x22;
  a.length = 24;
  memset(a.bytes, 0, sizeof(a.bytes));
  gw_put_u32(a.bytes + 8, 1);
  gw_put_u16(a.bytes + 19, 1);
  assert(!gw_spool_add(&s, &a, 1, NULL));
  assert(gw_spool_state(&s, 0) == GW_SP_INBOX);
  gw_spool_pair_t pair = {0, GW_SPOOL_NONE};
  assert(gw_spool_finish(&s, &pair, 1) == GW_SP_FAULT);
  gw_spool_close(&s);
  fresh(f, &s);
  a = record(0, 6);
  assert(!gw_spool_add(&s, &a, 1, NULL));
  unsigned active = s.active;
  gw_spool_close(&s);
  f->bytes[active * GW_SPOOL_ROOT_BYTES + 24] ^= 1;
  gw_spool_io_t v = io(f);
  assert(gw_spool_open(&s, &v, session) == GW_SP_FAULT);
  gw_spool_close(&s);
  fresh(f, &s);
  assert(!gw_spool_add(&s, &a, 1, NULL));
  gw_spool_close(&s);
  f->bytes[GW_SPOOL_DATA_OFFSET + 96] ^= 1;
  assert(gw_spool_open(&s, &v, session) == GW_SP_FAULT);
  gw_spool_close(&s);
  memset(f->bytes, 255, sizeof(f->bytes));
  f->bytes[GW_SPOOL_BYTES - 1] = 0;
  assert(gw_spool_open(&s, &v, session) == GW_SP_FAULT);
  gw_spool_close(&s);
  free(f);
}
static void test_power_cuts(void) {
  flash_t *base = malloc(sizeof(*base)), *trial = malloc(sizeof(*trial));
  gw_spool_t s;
  fresh(base, &s);
  gw_spool_record_t a[2] = {record(0, 6), record(0, 7)};
  gw_spool_watermark_t one = watermark(1, 1, 1), two = watermark(2, 2, 2);
  assert(!gw_spool_add(&s, a, 1, &one));
  gw_spool_close(&s);
  /* Interrupt every IO in a complete two-observation batch, before, midway,
   * and after the physical operation. Recover either whole old or whole new. */
  for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned cut = 1; cut <= 14; cut++) {
      memcpy(trial, base, sizeof(*trial));
      trial->fail = 0;
      gw_spool_io_t v = io(trial);
      assert(!gw_spool_open(&s, &v, session));
      trial->op = 0;
      trial->fail = cut;
      trial->mode = mode;
      int add = gw_spool_add(&s, a, 2, &two);
      gw_spool_close(&s);
      trial->fail = 0;
      int opened = gw_spool_open(&s, &v, session);
      if (opened) {
        fprintf(stderr, "recover failed mode=%u cut=%u add=%d\n", mode, cut,
                add);
        exit(1);
      }
      unsigned pending = count_state(&s, GW_SP_PENDING);
      assert(pending == 1 || pending == 3);
      if (add == GW_SP_OK)
        assert(pending == 3);
      assert(gw_spool_check(&s, &two) ==
             (pending == 3 ? GW_SP_REPLAY : GW_SP_OK));
      gw_spool_close(&s);
    }
  /* Fill all slots, release one, and force its physical reuse. Old inactive
   * roots must be invalidated before that erase, including cut halfway erase.
   */
  gw_spool_io_t baseio = io(base);
  assert(!gw_spool_open(&s, &baseio, session));
  for (unsigned i = 1; i < GW_SPOOL_SLOTS; i++)
    assert(!gw_spool_add(&s, a, 1, NULL));
  gw_spool_pair_t pair = {0, GW_SPOOL_NONE};
  assert(!gw_spool_finish(&s, &pair, 1));
  uint16_t slot = 0;
  uint8_t state = GW_SP_FREE;
  assert(!gw_spool_mark(&s, &slot, &state, 1));
  gw_spool_close(&s);
  for (unsigned mode = 0; mode < 3; mode++)
    for (unsigned cut = 1; cut <= 9; cut++) {
      memcpy(trial, base, sizeof(*trial));
      trial->fail = 0;
      gw_spool_io_t v = io(trial);
      assert(!gw_spool_open(&s, &v, session));
      trial->op = 0;
      trial->fail = cut;
      trial->mode = mode;
      int add = gw_spool_add(&s, a, 1, NULL);
      gw_spool_close(&s);
      trial->fail = 0;
      assert(!gw_spool_open(&s, &v, session));
      unsigned count = count_state(&s, GW_SP_PENDING);
      assert(count == GW_SPOOL_SLOTS - 1 || count == GW_SPOOL_SLOTS);
      if (add == GW_SP_OK)
        assert(count == GW_SPOOL_SLOTS);
      gw_spool_close(&s);
    }
  free(base);
  free(trial);
}
static void test_pairing_rules(void) {
  gw_spool_record_t a = record(0, 6), b = record(1, 6);
  uint64_t delta, mapped;
  uint32_t error;
  assert(gw_capture_record_valid(0x20, a.bytes, a.length));
  assert(gw_capture_pairable(&a, &b, &delta) && delta == 0);
  b.radio = 0;
  assert(!gw_capture_pairable(&a, &b, &delta));
  b.radio = 1;
  b.mapped_ms = 946;
  assert(gw_capture_pairable(&a, &b, &delta));
  b.mapped_ms = 947;
  assert(!gw_capture_pairable(&a, &b, &delta));
  b.mapped_ms = 900;
  b.bytes[26] ^= 1;
  assert(!gw_capture_pairable(&a, &b, &delta));
  b.bytes[26] ^= 1;
  b.bytes[8] = 1;
  b.bytes[9] = 0;
  assert(!gw_capture_pairable(&a, &b, &delta));
  b = record(1, 6);
  a.bytes[10] = b.bytes[10] = 255;
  memset(a.bytes + 11, 0, 6);
  memset(b.bytes + 11, 0, 6);
  assert(!gw_capture_pairable(&a, &b, &delta));
  a = record(0, 6);
  b = record(1, 6);
  a.bytes[8] = b.bytes[8] = 2;
  assert(!gw_capture_pairable(&a, &b, &delta));
  a.bytes[21] = b.bytes[21] = 4;
  assert(gw_capture_pairable(&a, &b, &delta));
  assert(gw_capture_map_time(60100, 100, 1000, 2, &mapped, &error) &&
         mapped == 61000 && error == 9);
  assert(!gw_capture_map_time(60101, 100, 1000, 2, &mapped, &error));
  assert(!gw_capture_map_time(0, 100, 50, 2, &mapped, &error));
  a.bytes[22] = 99;
  assert(!gw_capture_record_valid(0x20, a.bytes, a.length));
}
int main(void) {
  test_replay_and_capacity();
  test_faults_and_inbox();
  test_power_cuts();
  test_pairing_rules();
  puts("capture: custody, replay, quarantine, full ring, 69 power cuts, "
       "corruption, inbox and pair/time rules passed");
  return 0;
}
