#include "gw_provision_io.h"

void gw_prov_sockets_init(gw_prov_sockets_t *s) {
    for (size_t i = 0; i < GW_PROV_SOCKET_MAX; ++i)
        s->fd[i] = -1;
}
bool gw_prov_socket_add(gw_prov_sockets_t *s, int fd) {
    if (fd < 0)
        return false;
    for (size_t i = 0; i < GW_PROV_SOCKET_MAX; ++i)
        if (s->fd[i] == fd)
            return false;
    for (size_t i = 0; i < GW_PROV_SOCKET_MAX; ++i)
        if (s->fd[i] < 0) {
            s->fd[i] = fd;
            return true;
        }
    return false;
}
void gw_prov_socket_remove(gw_prov_sockets_t *s, int fd) {
    for (size_t i = 0; i < GW_PROV_SOCKET_MAX; ++i)
        if (s->fd[i] == fd)
            s->fd[i] = -1;
}
void gw_prov_sockets_shutdown(const gw_prov_sockets_t *s,
                              void (*shutdown_fd)(void *, int), void *ctx) {
    for (size_t i = 0; i < GW_PROV_SOCKET_MAX; ++i)
        if (s->fd[i] >= 0)
            shutdown_fd(ctx, s->fd[i]);
}
int gw_prov_receive(const gw_prov_reader_t *r, uint8_t *data, size_t size,
                    uint64_t deadline_ms) {
    if (!r || !r->recv || !r->now_ms || !r->active || !data || !size)
        return GW_PROV_IO_FAILED;
    size_t received = 0;
    while (received < size) {
        if (!r->active(r->ctx) || r->now_ms(r->ctx) >= deadline_ms)
            return GW_PROV_IO_EXPIRED;
        int n = r->recv(r->ctx, data + received, size - received);
        if (!r->active(r->ctx) || r->now_ms(r->ctx) >= deadline_ms)
            return GW_PROV_IO_EXPIRED;
        if (n <= 0 || (size_t)n > size - received)
            return GW_PROV_IO_FAILED;
        received += (size_t)n;
    }
    return 0;
}
