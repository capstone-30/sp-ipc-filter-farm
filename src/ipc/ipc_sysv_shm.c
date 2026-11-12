#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"
#include "ipc_meta.h"
#include "ipc_owned.h"
#include "ipc_names.h"
#include "ipc_util.h"
#include "shm_ring.h"

struct ipc_channel {
    shm_ring_t r;
    int record_fd, owned, record_owned;
    char key[496], record[512];
    ipc_record_t resources;
};
union sem_arg { int val; struct semid_ds *buf; unsigned short *array; };

static void destroy(ipc_channel_t *ch)
{
    if (!ch || !ch->r.server || !ch->owned) return;
    if (owned_remove(OWN_SHM, &ch->resources) < 0) return;
    if (ch->record_owned && ipc_unlink(ch->record) < 0) return;
    if (ch->resources.key_owned && (ch->record_owned || ch->record_fd < 0) && ipc_unlink(ch->key) < 0) return;
    if (meta_remove(ch->key) < 0) return;
    ch->owned = 0;
}
static void close_channel(ipc_channel_t *ch)
{
    if (!ch) return;
    int e = errno;
    if (ch->r.mem && shmdt(ch->r.mem) < 0) e = errno;
    if (ch->r.meta >= 0 && close(ch->r.meta) < 0) e = errno;
    if (ch->record_fd >= 0 && close(ch->record_fd) < 0) e = errno;
    errno = e; ring_close(&ch->r); e = errno;
    free(ch); errno = e;
}
static int limit(const char *path, uint64_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    unsigned long long value;
    int rc = fscanf(f, "%llu", &value), e = errno;
    if (fclose(f) < 0) return -1;
    if (rc != 1) { errno = e ? e : EIO; return -1; }
    *out = (uint64_t)value; return 0;
}
static int create(ipc_channel_t *ch)
{
    uint64_t max, pages;
    if (limit("/proc/sys/kernel/shmmax", &max) < 0 || limit("/proc/sys/kernel/shmall", &pages) < 0) return -1;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) { errno = EINVAL; return -1; }
    if (ch->r.bytes > max || (ch->r.bytes + (size_t)page - 1) / (size_t)page > pages) { errno = ENOSPC; return -1; }
    int fd = open(ch->key, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd >= 0) { ch->resources.key_owned = 1; if (close(fd) < 0) return -1; }
    else if (errno != EEXIST) return -1;
    ch->record_fd = open(ch->record, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (ch->record_fd < 0) return -1;
    ipc_record_t old;
    int loaded = owned_read(ch->record_fd, &old);
    if (loaded < 0) return -1;
    if (loaded) {
        if (old.keys[0] != ch->resources.keys[0] || old.keys[1] != ch->resources.keys[1]) { errno = EEXIST; return -1; }
        if (owned_remove(OWN_SHM, &old) < 0) return -1;
        ch->resources.key_owned |= old.key_owned;
    }
    ch->record_owned = 1;
    if (owned_write(ch->record_fd, &ch->resources) < 0) return -1;
    ch->resources.ids[0] = shmget(ch->resources.keys[0], ch->r.bytes, IPC_CREAT | IPC_EXCL | 0600);
    if (ch->resources.ids[0] < 0 || owned_write(ch->record_fd, &ch->resources) < 0) return -1;
    ch->resources.ids[1] = semget(ch->resources.keys[1], SHM_SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    if (ch->resources.ids[1] < 0 || owned_write(ch->record_fd, &ch->resources) < 0) return -1;
    unsigned short counts[SHM_SEM_COUNT] = {0};
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        counts[SHM_SEM(i, SEM_REQ_EMPTY)] = (unsigned short)ch->r.depth;
        counts[SHM_SEM(i, SEM_RESP_EMPTY)] = (unsigned short)ch->r.depth;
    }
    union sem_arg arg = {.array = counts};
    return semctl(ch->resources.ids[1], 0, SETALL, arg);
}
static int sysv_step(shm_ring_t *r, unsigned id, int which, short change)
{
    ipc_channel_t *ch = r->context;
    struct sembuf op = {.sem_num = (unsigned short)SHM_SEM(id - 1, which), .sem_op = change, .sem_flg = IPC_NOWAIT};
    int rc = semop(ch->resources.ids[1], &op, 1);
    if (rc < 0 && !r->server && (errno == EIDRM || errno == EINVAL)) errno = ECONNRESET;
    return rc;
}
static ipc_channel_t *open_channel(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    if (!key || !ipc_config_valid(cfg) || id > SLOT_COUNT) { errno = EINVAL; return NULL; }
    if (strlen(key) >= sizeof ((ipc_channel_t *)0)->key) { errno = ENAMETOOLONG; return NULL; }
    ipc_channel_t *ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    if (ring_init(&ch->r, id, cfg, sizeof(shm_hdr_t)) < 0) { free(ch); return NULL; }
    ch->r.context = ch; ch->r.step = sysv_step;
    ch->record_fd = -1;
    ch->resources = (ipc_record_t){ .magic = IPC_MAGIC, .ids = {-1, -1} };
    for (unsigned i = 0; i < 2; i++) ch->resources.keys[i] = ipc_team_key(cfg->team, i + 2);
    memcpy(ch->key, key, strlen(key) + 1);
    int e;
    int n = snprintf(ch->record, sizeof ch->record, "%s.shm", key);
    if (n < 0 || (size_t)n >= sizeof ch->record) { errno = ENAMETOOLONG; goto fail; }
    ch->r.meta = meta_open(key);
    if (ch->r.meta < 0 || meta_trylock(ch->r.meta, id) < 0) goto fail;
    ch->owned = ch->r.server;
    meta_t m;
    if (meta_begin(ch->r.meta, id, "sysv_shm", cfg, &m) < 0) goto fail;
    if (ch->r.server) {
        if (create(ch) < 0) goto fail;
    } else {
        ch->r.server_pid = m.server_pid;
        ch->resources.ids[0] = shmget(ch->resources.keys[0], 0, 0);
        ch->resources.ids[1] = semget(ch->resources.keys[1], 0, 0);
        if (ch->resources.ids[0] < 0 || ch->resources.ids[1] < 0) goto fail;
    }
    struct shmid_ds ds; struct semid_ds semds; union sem_arg arg = {.buf = &semds};
    if (shmctl(ch->resources.ids[0], IPC_STAT, &ds) < 0 || semctl(ch->resources.ids[1], 0, IPC_STAT, arg) < 0) goto fail;
    if (ds.shm_segsz != ch->r.bytes || semds.sem_nsems != SHM_SEM_COUNT || ds.shm_cpid != m.server_pid) {
        errno = EINVAL; goto fail;
    }
    void *mem = shmat(ch->resources.ids[0], NULL, 0);
    if (mem == (void *)-1) goto fail;
    ch->r.mem = mem;
    shm_hdr_t expected = {.magic = IPC_MAGIC, .slot_count = SLOT_COUNT, .depth = cfg->depth, .slot_bytes = ch->r.payload};
    if (ch->r.server) {
        memcpy(ch->r.mem, &expected, sizeof expected);
        m.ready = 1; if (meta_store(ch->r.meta, &m) < 0) goto fail;
    } else if (memcmp(ch->r.mem, &expected, sizeof expected)) { errno = EINVAL; goto fail; }
    return ch;
fail:
    e = errno ? errno : EIO; destroy(ch); close_channel(ch); errno = e; return NULL;
}
static int send_message(ipc_channel_t *ch, const msg_hdr_t *h, const void *p) { return ring_send(&ch->r, h, p); }
static int receive_message(ipc_channel_t *ch, msg_hdr_t *h, void *p, size_t cap) { return ring_recv(&ch->r, h, p, cap); }
static ipc_channel_t *server_open(const char *key, const ipc_config_t *cfg) { return open_channel(key, 0, cfg); }
static ipc_channel_t *client_open(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    if (!id) { errno = EINVAL; return NULL; }
    return open_channel(key, id, cfg);
}
const ipc_backend_t ipc_backend_sysv_shm = {
    .name = "sysv_shm", .server_open = server_open, .client_open = client_open,
    .send = send_message, .recv = receive_message, .close = close_channel, .destroy = destroy
};
