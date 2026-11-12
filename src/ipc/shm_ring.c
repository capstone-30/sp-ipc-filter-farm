#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shm_ring.h"
#include "ipc_meta.h"
#include "ipc_util.h"

int ring_init(shm_ring_t *r, uint32_t id, const ipc_config_t *cfg, size_t header_bytes)
{
    if (!ipc_config_valid(cfg) || id > SLOT_COUNT) { errno = EINVAL; return -1; }
    r->id = id; r->server = !id; r->depth = cfg->depth; r->meta = -1;
    for (unsigned i = 0; i < SLOT_COUNT; i++) atomic_init(&r->dead[i], false);
    uint64_t payload = ipc_payload_size(cfg);
    uint64_t stride = (sizeof(msg_hdr_t) + payload + _Alignof(msg_hdr_t) - 1) & ~(uint64_t)(_Alignof(msg_hdr_t) - 1);
    uint64_t bytes = header_bytes + 2 * SLOT_COUNT * cfg->depth * stride;
    if (payload > MAX_PAYLOAD || bytes > SIZE_MAX) { errno = EOVERFLOW; return -1; }
    r->payload = (size_t)payload; r->stride = (size_t)stride; r->bytes = (size_t)bytes; r->offset = header_bytes;
    int e = pthread_mutex_init(&r->tx, NULL);
    if (e) { errno = e; return -1; }
    e = pthread_mutex_init(&r->rx, NULL);
    if (e) { int rc = pthread_mutex_destroy(&r->tx); (void)rc; errno = e; return -1; }
    return 0;
}
void ring_close(shm_ring_t *r)
{
    int e = errno, rc = pthread_mutex_destroy(&r->tx); if (rc) e = rc;
    rc = pthread_mutex_destroy(&r->rx); if (rc) e = rc;
    errno = e;
}
static int step(shm_ring_t *r, unsigned id, int which, short change)
{
    if (change < 0 && atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
    int rc = r->step(r, id, which, change);
    if (!rc && change < 0 && atomic_load(&g_stop)) {
        if (r->step(r, id, which, 1) < 0) return -1;
        errno = ECANCELED; return -1;
    }
    return rc;
}
static int peer(shm_ring_t *ch, unsigned id)
{
    if (atomic_load(&ch->dead[id - 1])) { errno = ch->server ? EPIPE : ECONNRESET; return -1; }
    errno = 0; pid_t pid = meta_owner(ch->meta, ch->server ? id : 0);
    if (!pid && errno) return -1;
    int retired = meta_id_retired(ch->meta, id);
    if (retired < 0) return -1;
    if (!pid || (!ch->server && pid != ch->server_pid) || retired) {
        if (ch->server && meta_retire_id(ch->meta, id) < 0) return -1;
        atomic_store(&ch->dead[id - 1], true);
        errno = ch->server ? EPIPE : ECONNRESET; return -1;
    }
    return 0;
}
static uint8_t *slot(shm_ring_t *ch, unsigned id, int response, uint64_t index)
{
    size_t position = ((size_t)response * SLOT_COUNT + id - 1) * ch->depth + index % ch->depth;
    return ch->mem + ch->offset + position * ch->stride;
}
int ring_send(shm_ring_t *ch, const msg_hdr_t *h, const void *payload)
{
    if (!h || !h->client_id || h->client_id > SLOT_COUNT || h->payload_len > ch->payload ||
        h->payload_len > MAX_PAYLOAD || (h->payload_len && !payload) || (!ch->server && h->client_id != ch->id)) {
        errno = EINVAL; return -1;
    }
    int e = pthread_mutex_lock(&ch->tx); if (e) { errno = e; return -1; }
    unsigned id = h->client_id;
    int empty = ch->server ? SEM_RESP_EMPTY : SEM_REQ_EMPTY, full = empty + 1;
    uint64_t checked = 0, t;
    for (;;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; break; }
        if (ipc_now(&t) < 0) break;
        if (!checked || t - checked >= 100000000) { if (peer(ch, id) < 0) break; checked = t; }
        if (step(ch, id, empty, -1) == 0) {
            uint8_t *p = slot(ch, id, ch->server, ch->tail[id - 1]++);
            memcpy(p, h, sizeof *h);
            if (h->payload_len) memcpy(p + sizeof *h, payload, h->payload_len);
            return ipc_unlock(&ch->tx, step(ch, id, full, 1));
        }
        if (errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) break;
        if (!ch->timed && ipc_nap() < 0) break;
    }
    return ipc_unlock(&ch->tx, -1);
}
int ring_recv(shm_ring_t *ch, msg_hdr_t *h, void *payload, size_t cap)
{
    if (!h || (cap && !payload)) { errno = EINVAL; return -1; }
    int e = pthread_mutex_lock(&ch->rx); if (e) { errno = e; return -1; }
    uint64_t checked = 0, t;
    for (;;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; break; }
        if (ipc_now(&t) < 0) break;
        if (!checked || t - checked >= 100000000) {
            if (!ch->server) { if (peer(ch, ch->id) < 0) break; }
            else {
                meta_t m; if (meta_load(ch->meta, &m) < 0) break;
                for (unsigned id = 1; id <= SLOT_COUNT; id++)
                    if (m.used[id - 1] && peer(ch, id) < 0 && errno != EPIPE) goto out;
            }
            checked = t;
        }
        unsigned attempts = ch->server ? SLOT_COUNT : 1;
        for (unsigned i = 0; i < attempts; i++) {
            unsigned id = ch->server ? (ch->next++ % SLOT_COUNT) + 1 : ch->id;
            if (!ch->server && atomic_load(&ch->dead[id - 1])) { errno = ECONNRESET; goto out; }
            int full = ch->server ? SEM_REQ_FULL : SEM_RESP_FULL;
            if (step(ch, id, full, -1) < 0) {
                if (errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT) continue;
                goto out;
            }
            uint8_t *p = slot(ch, id, !ch->server, ch->head[id - 1]++);
            memcpy(h, p, sizeof *h);
            int valid = h->magic == IPC_MAGIC && h->client_id == id && h->payload_len <= ch->payload &&
                h->payload_len <= MAX_PAYLOAD && h->payload_len <= cap;
            /* 클라가 BYE 게시 직후 닫아도 이미 게시된 종료 통보는 전달한다. */
            if (atomic_load(&ch->dead[id - 1]) && !(h->type == MSG_BYE && !h->payload_len)) valid = 0;
            if (valid && h->payload_len) memcpy(payload, p + sizeof *h, h->payload_len);
            if (step(ch, id, full - 1, 1) < 0) goto out;
            if (!valid || (h->type == MSG_BYE && !h->payload_len)) {
                atomic_store(&ch->dead[id - 1], true);
                if (ch->server && meta_retire_id(ch->meta, id) < 0) goto out;
            }
            if (valid) return ipc_unlock(&ch->rx, 0);
            if (!ch->server) { errno = EPROTO; goto out; }
        }
        if ((ch->server || !ch->timed) && ipc_nap() < 0) break;
    }
out:
    return ipc_unlock(&ch->rx, -1);
}
