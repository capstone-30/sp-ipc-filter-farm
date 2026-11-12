#ifndef MQ_INTERNAL_H
#define MQ_INTERNAL_H
#include <pthread.h>
#include <sys/ipc.h>
#include "ipc.h"
#include "ipc_meta.h"
#include "ipc_sysv_mq.h"

typedef struct {
    uint8_t *data, *seen;
    uint32_t seq, count, have;
    size_t capacity, total;
    uint64_t last;
} mq_partial_t;

struct ipc_channel {
    int server, meta_fd, record_fd, q[2], owned, key_owned, record_owned;
    uint32_t id, depth;
    pid_t server_pid;
    char key[496], record[512];
    key_t keys[2];
    pthread_mutex_t tx, rx;
    atomic_bool dead[SLOT_COUNT];
    uint8_t bye_blocked[SLOT_COUNT];
    mq_partial_t partial[SLOT_COUNT][16];
};

ipc_channel_t *mq_open(const char *key, const ipc_config_t *cfg, uint32_t id);
void mq_close(ipc_channel_t *ch);
void mq_destroy(ipc_channel_t *ch);
int mq_peers(ipc_channel_t *ch);
int mq_retire(ipc_channel_t *ch, uint32_t id);
void mq_clear(mq_partial_t *p);
#endif
