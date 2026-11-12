#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mq_internal.h"

#include "ipc_owned.h"
#include "ipc_names.h"
#include "ipc_util.h"

void mq_clear(mq_partial_t *p)
{
    free(p->data); free(p->seen); memset(p, 0, sizeof *p);
}

static int save_record(ipc_channel_t *ch)
{
    ipc_record_t r = { .magic = IPC_MAGIC, .ids = {ch->q[0], ch->q[1]},
        .keys = {ch->keys[0], ch->keys[1]}, .key_owned = ch->key_owned };
    return owned_write(ch->record_fd, &r);
}

static int remove_queue(int q)
{
    if (q < 0 || msgctl(q, IPC_RMID, NULL) == 0) return 0;
    return errno == EINVAL || errno == EIDRM ? 0 : -1;
}

void mq_destroy(ipc_channel_t *ch)
{
    if (!ch || !ch->server || !ch->owned) return;
    int saved = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (remove_queue(ch->q[i]) < 0) saved = errno;
        else ch->q[i] = -1;
    }
    /* 실패한 회수를 재시도할 수 있도록 소유 기록을 남긴다. */
    if (!saved && ch->record_owned && ipc_unlink(ch->record) < 0) saved = errno;
    if (!saved && ch->key_owned && (ch->record_fd < 0 || ch->record_owned) && ipc_unlink(ch->key) < 0) saved = errno;
    if (!saved && meta_remove(ch->key) < 0) saved = errno;
    if (saved) errno = saved;
}

void mq_close(ipc_channel_t *ch)
{
    if (!ch) return;
    int saved = errno;
    for (unsigned i = 0; i < SLOT_COUNT; i++)
        for (unsigned j = 0; j < 16; j++) mq_clear(&ch->partial[i][j]);
    if (ch->record_fd >= 0 && close(ch->record_fd) < 0) saved = errno;
    if (ch->meta_fd >= 0 && close(ch->meta_fd) < 0) saved = errno;
    int e = pthread_mutex_destroy(&ch->tx); if (e) saved = e;
    e = pthread_mutex_destroy(&ch->rx); if (e) saved = e;
    free(ch); errno = saved;
}

static int create_queues(ipc_channel_t *ch)
{
    int fd = open(ch->key, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd >= 0) { ch->key_owned = 1; if (close(fd) < 0) return -1; }
    else if (errno != EEXIST) return -1;
    ch->record_fd = open(ch->record, O_RDWR | O_CREAT, 0600);
    if (ch->record_fd < 0) return -1;
    ipc_record_t old;
    int loaded = owned_read(ch->record_fd, &old);
    if (loaded < 0) return -1;
    if (loaded) {
        if (old.keys[0] != ch->keys[0] || old.keys[1] != ch->keys[1]) { errno = EEXIST; return -1; }
        if (owned_remove(OWN_MQ, &old) < 0) return -1;
        ch->key_owned |= old.key_owned;
    }
    ch->record_owned = 1;
    if (save_record(ch) < 0) return -1;
    for (unsigned i = 0; i < 2; i++) {
        ch->q[i] = msgget(ch->keys[i], IPC_CREAT | IPC_EXCL | 0600);
        if (ch->q[i] < 0) return -1;
        if (save_record(ch) < 0) return -1;
    }
    return 0;
}

ipc_channel_t *mq_open(const char *key, const ipc_config_t *cfg, uint32_t id)
{
    if (!key || !ipc_config_valid(cfg) || id > SLOT_COUNT) { errno = EINVAL; return NULL; }
    if (strlen(key) >= sizeof ((ipc_channel_t *)0)->key) { errno = ENAMETOOLONG; return NULL; }
    ipc_channel_t *ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    ch->meta_fd = ch->record_fd = ch->q[0] = ch->q[1] = -1;
    for (unsigned i = 0; i < 2; i++) ch->keys[i] = ipc_team_key(cfg->team, i);
    ch->id = id; ch->server = id == 0; ch->depth = cfg->depth;
    for (unsigned i = 0; i < SLOT_COUNT; i++) atomic_init(&ch->dead[i], false);
    memcpy(ch->key, key, strlen(key) + 1);
    int e = pthread_mutex_init(&ch->tx, NULL);
    if (e) { free(ch); errno = e; return NULL; }
    e = pthread_mutex_init(&ch->rx, NULL);
    if (e) { int ignored = pthread_mutex_destroy(&ch->tx); (void)ignored; free(ch); errno = e; return NULL; }
    int n = snprintf(ch->record, sizeof ch->record, "%s.mq", key);
    if (n < 0 || (size_t)n >= sizeof ch->record) { errno = ENAMETOOLONG; goto fail; }
    ch->meta_fd = meta_open(key);
    if (ch->meta_fd < 0 || meta_trylock(ch->meta_fd, id) < 0) goto fail;
    ch->owned = ch->server;
    meta_t m;
    if (meta_begin(ch->meta_fd, id, "sysv_mq", cfg, &m) < 0) goto fail;
    if (ch->server) {
        if (create_queues(ch) < 0) goto fail;
        m.ready = 1;
        if (meta_store(ch->meta_fd, &m) < 0) goto fail;
    } else {
        ch->server_pid = m.server_pid;
        for (unsigned i = 0; i < 2; i++) {
            ch->q[i] = msgget(ch->keys[i], 0);
            if (ch->q[i] < 0) goto fail;
        }
    }
    return ch;
fail:
    e = errno ? errno : EIO;
    /* 기록 검증 실패 시 타인의 소유 기록/큐를 제거하지 않는다. */
    mq_destroy(ch);
    mq_close(ch); errno = e; return NULL;
}

int mq_retire(ipc_channel_t *ch, uint32_t id)
{
    atomic_store(&ch->dead[id - 1], true);
    if (ch->server && meta_retire_id(ch->meta_fd, id) < 0) return -1;
    return 0;
}

int mq_peers(ipc_channel_t *ch)
{
    if (!ch->server) {
        errno = 0;
        pid_t pid = meta_owner(ch->meta_fd, 0);
        if (!pid && errno) return -1;
        if (!pid || pid != ch->server_pid) { errno = ECONNRESET; return -1; }
        int retired = meta_id_retired(ch->meta_fd, ch->id);
        if (retired < 0) return -1;
        if (retired) { errno = ECONNRESET; return -1; }
        return 0;
    }
    meta_t m;
    if (meta_load(ch->meta_fd, &m) < 0) return -1;
    for (uint32_t id = 1; id <= SLOT_COUNT; id++) {
        if (!m.used[id - 1]) continue;
        errno = 0;
        pid_t pid = meta_owner(ch->meta_fd, id);
        if (!pid && errno) return -1;
        if (!pid && mq_retire(ch, id) < 0) return -1;
        if (!atomic_load(&ch->dead[id - 1])) continue;
        mq_chunk_t garbage;
        /* 송신 재시도에서도 죽은 ID 응답을 비워 공유 응답 큐의 막힘을 해소한다. */
        for (unsigned i = 0; i < 64 && !atomic_load(&g_stop); i++) {
            if (msgrcv(ch->q[1], &garbage, MQ_MSGSZ(CHUNK_PAYLOAD), id,
                       IPC_NOWAIT | MSG_NOERROR) >= 0) continue;
            if (errno == EINTR) continue;
            if (errno == ENOMSG) break;
            return -1;
        }
    }
    return 0;
}
