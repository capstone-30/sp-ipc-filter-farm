#ifndef SHM_RING_H
#define SHM_RING_H
#include <pthread.h>
#include <sys/types.h>
#include "ipc.h"
#include "ipc_sysv_shm.h"

typedef struct shm_ring {
    int server, meta, timed;
    uint32_t id, depth, next;
    pid_t server_pid;
    size_t payload, stride, bytes, offset;
    uint8_t *mem;
    pthread_mutex_t tx, rx;
    atomic_bool dead[SLOT_COUNT];
    uint64_t head[SLOT_COUNT], tail[SLOT_COUNT];
    void *context;
    int (*step)(struct shm_ring *, unsigned, int, short);
} shm_ring_t;
int ring_init(shm_ring_t *r, uint32_t id, const ipc_config_t *cfg, size_t header_bytes);
void ring_close(shm_ring_t *r);
int ring_send(shm_ring_t *r, const msg_hdr_t *h, const void *payload);
int ring_recv(shm_ring_t *r, msg_hdr_t *h, void *payload, size_t cap);
#endif
