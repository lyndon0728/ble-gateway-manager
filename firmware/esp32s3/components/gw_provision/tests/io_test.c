#include "../gw_provision_io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t now, advance;
    size_t chunk;
    unsigned calls, close_on_call;
    bool open, read_error;
} fake_t;
static int receive(void *ctx, uint8_t *out, size_t n) {
    fake_t *f = ctx;
    ++f->calls;
    f->now += f->advance;
    if (f->close_on_call == f->calls)
        f->open = false;
    if (f->read_error)
        return -1;
    if (n > f->chunk)
        n = f->chunk;
    memset(out, (int)f->calls, n);
    return (int)n;
}
static uint64_t now(void *ctx) { return ((fake_t *)ctx)->now; }
static bool active(void *ctx) { return ((fake_t *)ctx)->open; }
static unsigned interrupted[32];
static void stop_fd(void *unused, int fd) {
    (void)unused;
    assert(fd >= 0 && fd < 32);
    interrupted[fd]++;
}
int main(void) {
    uint8_t bytes[16];
    fake_t f = {.open = true, .chunk = 2, .advance = 10};
    gw_prov_reader_t reader = {.recv = receive, .now_ms = now, .active = active, .ctx = &f};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == 0 && f.calls == 8);
    assert(bytes[0] == 1 && bytes[15] == 8);
    /* Each recv succeeds before its 5-second socket timeout, but the overall
     * request still stops at 15 seconds. It cannot drip-feed for the window. */
    f = (fake_t){.open = true, .chunk = 1, .advance = 4000};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_EXPIRED);
    assert(f.calls == 4);
    f = (fake_t){.open = true, .chunk = sizeof(bytes), .advance = 15000};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_EXPIRED);
    assert(f.calls == 1); /* A late final chunk cannot reach authentication. */
    f = (fake_t){.open = true, .chunk = sizeof(bytes), .close_on_call = 1};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_EXPIRED);
    f = (fake_t){.open = false, .chunk = 1};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_EXPIRED && !f.calls);
    f = (fake_t){.open = true, .chunk = 1, .read_error = true};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_FAILED);
    f = (fake_t){.open = true, .chunk = 0};
    assert(gw_prov_receive(&reader, bytes, sizeof(bytes), 15000) == GW_PROV_IO_FAILED);
    gw_prov_sockets_t sockets;
    gw_prov_sockets_init(&sockets);
    assert(gw_prov_socket_add(&sockets, 5));
    assert(gw_prov_socket_add(&sockets, 6));
    assert(gw_prov_socket_add(&sockets, 7));
    assert(!gw_prov_socket_add(&sockets, 5) && !gw_prov_socket_add(&sockets, 8));
    gw_prov_sockets_shutdown(&sockets, stop_fd, NULL);
    assert(interrupted[5] == 1 && interrupted[6] == 1 && interrupted[7] == 1);
    assert(!gw_prov_socket_add(&sockets, 8)); /* shutdown doesn't free/reuse fds */
    gw_prov_socket_remove(&sockets, 6); /* Only the HTTP close callback removes. */
    assert(gw_prov_socket_add(&sockets, 8));
    gw_prov_sockets_shutdown(&sockets, stop_fd, NULL);
    assert(interrupted[5] == 2 && interrupted[6] == 1 && interrupted[7] == 2 && interrupted[8] == 1);
    puts("PASS provisioning fixed body deadline, late/closed-window rejection and tracked socket interruption");
    return 0;
}
