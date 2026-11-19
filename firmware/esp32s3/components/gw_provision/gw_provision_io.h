#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_PROV_SOCKET_MAX 3
#define GW_PROV_IO_FAILED (-1)
#define GW_PROV_IO_EXPIRED (-2)

/* Called with the adapter's socket mutex held. Shutdown does not close or
 * forget descriptors: the HTTP task owns close(), preventing fd reuse races. */
typedef struct { int fd[GW_PROV_SOCKET_MAX]; } gw_prov_sockets_t;
void gw_prov_sockets_init(gw_prov_sockets_t *s);
bool gw_prov_socket_add(gw_prov_sockets_t *s, int fd);
void gw_prov_socket_remove(gw_prov_sockets_t *s, int fd);
void gw_prov_sockets_shutdown(const gw_prov_sockets_t *s,
                              void (*shutdown_fd)(void *, int), void *ctx);

typedef struct {
    int (*recv)(void *, uint8_t *, size_t);
    uint64_t (*now_ms)(void *);
    bool (*active)(void *);
    void *ctx;
} gw_prov_reader_t;
/* Fixed deadline: receiving another byte never extends the request budget.
 * Recheck after recv so a late final chunk is never authenticated/dispatched. */
int gw_prov_receive(const gw_prov_reader_t *reader, uint8_t *data, size_t size,
                    uint64_t deadline_ms);
