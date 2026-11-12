#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "shm_ring.h"
#include "ipc_meta.h"
#include "ipc_posix_shm.h"
#include "ipc_util.h"
#include "pshm_owned.h"

struct ipc_channel {
    shm_ring_t r;
    int fd, record_fd, owned, created, record_owned;
    unsigned initialized;
    char key[496], record[512];
    pshm_record_t resource;
};
/* sem_t 배열로 캐스팅하지 않고 계약의 개별 멤버를 선택한다. */
static sem_t *semaphore(shm_ring_t *r, unsigned id, int which)
{
    pshm_hdr_t *h = (pshm_hdr_t *)r->mem;
    switch (which) {
    case SEM_REQ_EMPTY: return &h->sems[id - 1].req_empty;
    case SEM_REQ_FULL: return &h->sems[id - 1].req_full;
    case SEM_RESP_EMPTY: return &h->sems[id - 1].resp_empty;
    default: return &h->sems[id - 1].resp_full;
    }
}
static int posix_step(shm_ring_t *r, unsigned id, int which, short change)
{
    sem_t *s = semaphore(r, id, which);
    if (change > 0) return sem_post(s);
    if (r->server && which == SEM_REQ_FULL) return sem_trywait(s);
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) < 0) return -1;
    t.tv_nsec += 100000000;
    if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
    return sem_timedwait(s, &t);
}
static void destroy(ipc_channel_t *ch)
{
    if (!ch || !ch->r.server || !ch->owned) return;
    if (ch->record_fd >= 0 && !ch->record_owned) return; /* 확인되지 않은 기존 기록·meta 보존 */
    int exclusive = 1;
    for (unsigned id = 1; id <= SLOT_COUNT; id++) {
        if (meta_trylock(ch->r.meta, id) < 0) {
            if (errno != EACCES && errno != EAGAIN) return;
            exclusive = 0;
        }
    }
    if (exclusive && ch->r.mem) {
        while (ch->initialized) {
            unsigned i = ch->initialized - 1;
            if (sem_destroy(semaphore(&ch->r, i / SEM_PER_SLOT + 1, i % SEM_PER_SLOT)) < 0) return;
            ch->initialized--;
        }
    }
    /* 클라가 살아 있으면 semaphore는 그대로 두고 이름만 제거한다. */
    if (ch->created && pshm_remove(&ch->resource) < 0) return;
    if (ch->record_owned && ipc_unlink(ch->record) < 0) return;
    if (meta_remove(ch->key) < 0) return;
    ch->owned = 0;
}
static void close_channel(ipc_channel_t *ch)
{
    if (!ch) return;
    int e = errno;
    if (ch->r.mem && munmap(ch->r.mem, ch->r.bytes) < 0) e = errno;
    if (ch->fd >= 0 && close(ch->fd) < 0) e = errno;
    if (ch->record_fd >= 0 && close(ch->record_fd) < 0) e = errno;
    if (ch->r.meta >= 0 && close(ch->r.meta) < 0) e = errno;
    errno = e; ring_close(&ch->r); e = errno;
    free(ch); errno = e;
}
static int create(ipc_channel_t *ch)
{
    ch->record_fd = open(ch->record, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (ch->record_fd < 0) return -1;
    pshm_record_t old;
    int loaded = pshm_read(ch->record_fd, &old);
    if (loaded < 0) return -1;
    if (loaded) {
        if (strcmp(old.name, ch->resource.name)) { errno = EEXIST; return -1; }
        if (pshm_remove(&old) < 0) return -1;
    }
    ch->record_owned = 1;
    ch->fd = shm_open(ch->resource.name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (ch->fd < 0) return -1;
    struct stat st;
    if (fstat(ch->fd, &st) < 0) {
        int e = errno;
        if (shm_unlink(ch->resource.name) < 0) return -1;
        errno = e; return -1;
    }
    ch->resource.device = (uint64_t)st.st_dev; ch->resource.inode = (uint64_t)st.st_ino;
    ch->created = 1;
    if (pshm_write(ch->record_fd, &ch->resource) < 0) return -1;
    if (ftruncate(ch->fd, (off_t)ch->r.bytes) < 0) return -1;
    /* DECISION: tmpfs 공간을 미리 확보해 송수신 중 SIGBUS를 방지한다. */
    int e = posix_fallocate(ch->fd, 0, (off_t)ch->r.bytes);
    if (e) { errno = e; return -1; }
    return 0;
}
static ipc_channel_t *open_channel(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    if (!key || !*key) { errno = EINVAL; return NULL; }
    if (strlen(key) >= sizeof ((ipc_channel_t *)0)->key) { errno = ENAMETOOLONG; return NULL; }
    ipc_channel_t *ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    if (ring_init(&ch->r, id, cfg, sizeof(pshm_hdr_t)) < 0) { free(ch); return NULL; }
    ch->r.timed = 1; ch->r.step = posix_step;
    ch->fd = ch->record_fd = -1; ch->resource.magic = IPC_MAGIC;
    memcpy(ch->key, key, strlen(key) + 1);
    int e, n = snprintf(ch->record, sizeof ch->record, "%s.pshm", key);
    if (n < 0 || (size_t)n >= sizeof ch->record) { errno = ENAMETOOLONG; goto fail; }
    ch->r.meta = meta_open(key);
    if (ch->r.meta < 0 || meta_trylock(ch->r.meta, id) < 0) goto fail;
    if (pshm_name(cfg->team, ch->resource.name) < 0) goto fail;
    ch->owned = ch->r.server;
    meta_t m;
    if (meta_begin(ch->r.meta, id, "posix_shm", cfg, &m) < 0) goto fail;
    if (ch->r.server) {
        if (create(ch) < 0) goto fail;
    } else {
        ch->r.server_pid = m.server_pid;
        ch->record_fd = open(ch->record, O_RDONLY | O_NOFOLLOW);
        if (ch->record_fd < 0) goto fail;
        pshm_record_t saved;
        int loaded = pshm_read(ch->record_fd, &saved);
        if (loaded < 0) goto fail;
        if (!loaded || strcmp(saved.name, ch->resource.name)) { errno = EPROTO; goto fail; }
        ch->resource = saved;
        ch->fd = shm_open(ch->resource.name, O_RDWR, 0);
        if (ch->fd < 0 || pshm_check(ch->fd, &ch->resource) < 0) goto fail;
    }
    struct stat st;
    if (fstat(ch->fd, &st) < 0) goto fail;
    if (st.st_size != (off_t)ch->r.bytes) { errno = EINVAL; goto fail; }
    void *mem = mmap(NULL, ch->r.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ch->fd, 0);
    if (mem == MAP_FAILED) goto fail;
    ch->r.mem = mem;
    pshm_hdr_t expected = {.magic = IPC_MAGIC, .slot_count = SLOT_COUNT,
        .depth = cfg->depth, .slot_bytes = ch->r.payload};
    if (ch->r.server) {
        memcpy(mem, &expected, offsetof(pshm_hdr_t, sems));
        for (unsigned i = 0; i < SHM_SEM_COUNT; i++) {
            unsigned value = i % 2 ? 0 : cfg->depth;
            if (sem_init(semaphore(&ch->r, i / SEM_PER_SLOT + 1, i % SEM_PER_SLOT), 1, value) < 0) goto fail;
            ch->initialized++;
        }
        m.ready = 1; if (meta_store(ch->r.meta, &m) < 0) goto fail;
    } else if (memcmp(mem, &expected, offsetof(pshm_hdr_t, sems))) { errno = EINVAL; goto fail; }
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
const ipc_backend_t ipc_backend_posix_shm = {
    .name = "posix_shm", .server_open = server_open, .client_open = client_open,
    .send = send_message, .recv = receive_message, .close = close_channel, .destroy = destroy
};
