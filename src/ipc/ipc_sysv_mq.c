#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <time.h>
#include "mq_internal.h"
#include "ipc_util.h"

#define PEER_NS 100000000ull
#define ASSEMBLY_NS 30000000000ull
#define MAX_CHUNKS ((sizeof(msg_hdr_t) + MAX_PAYLOAD + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD)

static ipc_channel_t *server_open(const char *key, const ipc_config_t *cfg)
{
    return mq_open(key, cfg, 0);
}

static ipc_channel_t *client_open(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    if (!id) { errno = EINVAL; return NULL; }
    return mq_open(key, cfg, id);
}

static int send_message(ipc_channel_t *ch, const msg_hdr_t *h, const void *payload)
{
    if (!h || h->payload_len > MAX_PAYLOAD || !h->client_id || h->client_id > SLOT_COUNT ||
        (h->payload_len && !payload) || (!ch->server && h->client_id != ch->id)) {
        errno = EINVAL; return -1;
    }
    int e = pthread_mutex_lock(&ch->tx);
    if (e) { errno = e; return -1; }
    size_t total = sizeof *h + h->payload_len;
    mq_chunk_t chunk = { .mtype = ch->server ? h->client_id : 1,
        .client_id = h->client_id, .seq = h->seq,
        .chunk_cnt = (uint32_t)((total + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD) };
    uint64_t checked = 0, now;
    for (size_t offset = 0; offset < total; offset += CHUNK_PAYLOAD) {
        chunk.chunk_idx = (uint32_t)(offset / CHUNK_PAYLOAD);
        chunk.bytes = (uint32_t)(total - offset > CHUNK_PAYLOAD ? CHUNK_PAYLOAD : total - offset);
        if (!offset) {
            memcpy(chunk.data, h, sizeof *h);
            if (chunk.bytes > sizeof *h) memcpy(chunk.data + sizeof *h, payload, chunk.bytes - sizeof *h);
        } else memcpy(chunk.data, (const uint8_t *)payload + offset - sizeof *h, chunk.bytes);
        for (;;) {
            if (atomic_load(&g_stop)) { errno = ECANCELED; goto fail; }
            if (atomic_load(&ch->dead[h->client_id - 1])) { errno = EPIPE; goto fail; }
            if (ipc_now(&now) < 0) goto fail;
            if (!checked || now - checked >= PEER_NS) {
                if (mq_peers(ch) < 0) goto fail;
                checked = now;
                if (atomic_load(&ch->dead[h->client_id - 1])) { errno = EPIPE; goto fail; }
            }
            if (msgsnd(ch->q[ch->server ? 1 : 0], &chunk, MQ_MSGSZ(chunk.bytes), IPC_NOWAIT) == 0) break;
            if (errno != EAGAIN && errno != EINTR) {
                if (!ch->server && (errno == EINVAL || errno == EIDRM)) errno = ECONNRESET;
                goto fail;
            }
            if (ipc_nap() < 0) goto fail;
        }
    }
    return ipc_unlock(&ch->tx, 0);
fail:
    return ipc_unlock(&ch->tx, -1);
}

static void clear_id(ipc_channel_t *ch, uint32_t id)
{
    for (unsigned j = 0; j < ch->depth; j++) mq_clear(&ch->partial[id - 1][j]);
}

static int retire(ipc_channel_t *ch, uint32_t id)
{
    clear_id(ch, id);
    return mq_retire(ch, id);
}

static int is_bye_chunk(const mq_chunk_t *c, ssize_t n)
{
    if (c->chunk_cnt != 1 || c->chunk_idx != 0 || c->bytes != sizeof(msg_hdr_t) ||
        (size_t)n != MQ_MSGSZ(sizeof(msg_hdr_t)))
        return 0;
    msg_hdr_t head;
    memcpy(&head, c->data, sizeof head);
    return head.magic == IPC_MAGIC && head.type == MSG_BYE && !head.payload_len &&
        head.client_id == c->client_id && head.seq == c->seq;
}

/* 0=미완성, 1=완성, -1=잘못된 프레이밍, -2=할당/시계 오류. */
static int assemble(ipc_channel_t *ch, const mq_chunk_t *c, ssize_t length,
                    msg_hdr_t *h, void *payload, size_t cap, uint64_t now)
{
    if (length < (ssize_t)MQ_MSGSZ(0) || c->bytes > CHUNK_PAYLOAD || !c->bytes ||
        (size_t)length != MQ_MSGSZ(c->bytes) || !c->chunk_cnt || c->chunk_cnt > MAX_CHUNKS ||
        c->chunk_idx >= c->chunk_cnt ||
        (c->chunk_idx + 1 < c->chunk_cnt && c->bytes != CHUNK_PAYLOAD)) return -1;
    size_t offset = (size_t)c->chunk_idx * CHUNK_PAYLOAD;
    size_t limit = sizeof *h + (cap < MAX_PAYLOAD ? cap : MAX_PAYLOAD);
    if (offset + c->bytes > limit || (size_t)(c->chunk_cnt - 1) * CHUNK_PAYLOAD >= limit) return -1;
    mq_partial_t *p = NULL, *empty = NULL;
    for (unsigned j = 0; j < ch->depth; j++) {
        mq_partial_t *candidate = &ch->partial[c->client_id - 1][j];
        if (candidate->data && candidate->seq == c->seq) p = candidate;
        if (!candidate->data && !empty) empty = candidate;
    }
    if (!p) {
        if (!empty) return -1;
        p = empty;
        size_t bytes = (size_t)c->chunk_cnt * CHUNK_PAYLOAD;
        p->capacity = bytes < limit ? bytes : limit;
        p->data = malloc(p->capacity);
        p->seen = calloc(c->chunk_cnt, 1);
        if (!p->data || !p->seen) { mq_clear(p); return -2; }
        p->seq = c->seq; p->count = c->chunk_cnt;
    }
    if (p->count != c->chunk_cnt || p->seen[c->chunk_idx] || offset + c->bytes > p->capacity) return -1;
    /* 최종 청크 또는 헤더가 먼저 도착해도 전체 길이는 서로 일치해야 한다. */
    if (c->chunk_idx + 1 == c->chunk_cnt) {
        if (p->total && p->total != offset + c->bytes) return -1;
        p->total = offset + c->bytes;
    }
    if (!c->chunk_idx) {
        if (c->bytes < sizeof *h) return -1;
        msg_hdr_t head;
        memcpy(&head, c->data, sizeof head);
        size_t total = sizeof head + (size_t)head.payload_len;
        if (head.magic != IPC_MAGIC || head.client_id != c->client_id || head.seq != c->seq ||
            head.payload_len > MAX_PAYLOAD || head.payload_len > cap ||
            (total + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD != c->chunk_cnt ||
            (p->total && p->total != total)) return -1;
        p->total = total;
    }
    memcpy(p->data + offset, c->data, c->bytes);
    p->seen[c->chunk_idx] = 1; p->have++; p->last = now;
    if (p->have != p->count) return 0;
    memcpy(h, p->data, sizeof *h);
    if (h->payload_len > cap || sizeof *h + h->payload_len != p->total) return -1;
    if (h->payload_len) memcpy(payload, p->data + sizeof *h, h->payload_len);
    mq_clear(p);
    if (h->type == MSG_BYE && h->payload_len == 0) {
        /* BYE 이후에는 더 이상 그 ID의 응답이나 재조립을 유지하지 않는다. */
        if (ch->server) ch->bye_blocked[h->client_id - 1] = 1;
        if (retire(ch, h->client_id) < 0) return -2;
    }
    return 1;
}

static int sweep(ipc_channel_t *ch, uint64_t now)
{
    if (mq_peers(ch) < 0) return -1;
    for (uint32_t id = 1; id <= SLOT_COUNT; id++) {
        if (atomic_load(&ch->dead[id - 1])) { clear_id(ch, id); continue; }
        for (unsigned j = 0; j < ch->depth; j++) {
            mq_partial_t *p = &ch->partial[id - 1][j];
            if (p->data && now - p->last >= ASSEMBLY_NS) {
                if (ch->server) ch->bye_blocked[id - 1] = 1;
                if (retire(ch, id) < 0) return -1;
                if (!ch->server) { errno = ETIMEDOUT; return -1; }
                break;
            }
        }
    }
    return 0;
}

static int receive_message(ipc_channel_t *ch, msg_hdr_t *h, void *payload, size_t cap)
{
    if (!h || (cap && !payload)) { errno = EINVAL; return -1; }
    int e = pthread_mutex_lock(&ch->rx);
    if (e) { errno = e; return -1; }
    uint64_t now, checked = 0;
    for (;;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; break; }
        if (ipc_now(&now) < 0) break;
        if (!checked || now - checked >= PEER_NS) {
            if (sweep(ch, now) < 0) break;
            checked = now;
        }
        /* 한 바이트 여유로 oversized 메시지도 소비해 큐 맨 앞 E2BIG 고착을 막는다. */
        struct { long type; uint8_t body[MQ_MSGSZ(CHUNK_PAYLOAD) + 1]; } raw;
        ssize_t n = msgrcv(ch->q[ch->server ? 0 : 1], &raw, sizeof raw.body,
                          ch->server ? 1 : ch->id, IPC_NOWAIT | MSG_NOERROR);
        if (n < 0) {
            if (errno == EINTR || errno == ENOMSG) { if (ipc_nap() < 0) break; continue; }
            if (!ch->server && (errno == EIDRM || errno == EINVAL)) errno = ECONNRESET;
            break;
        }
        if (n < (ssize_t)sizeof(uint32_t)) { errno = EPROTO; break; }
        mq_chunk_t chunk = { .mtype = raw.type };
        size_t copied = (size_t)n < MQ_MSGSZ(CHUNK_PAYLOAD) ? (size_t)n : MQ_MSGSZ(CHUNK_PAYLOAD);
        memcpy((char *)&chunk + sizeof(long), raw.body, copied);
        if (!chunk.client_id || chunk.client_id > SLOT_COUNT ||
            (!ch->server && chunk.client_id != ch->id)) { errno = EPROTO; break; }
        if (atomic_load(&ch->dead[chunk.client_id - 1])) {
            /* 클라가 BYE 송신 직후 종료해도 이미 큐에 적재된 정상 종료 통보는 전달한다. */
            if (!ch->server || ch->bye_blocked[chunk.client_id - 1] || !is_bye_chunk(&chunk, n))
                continue;
            clear_id(ch, chunk.client_id);
        }
        int rc = assemble(ch, &chunk, n, h, payload, cap, now);
        if (rc == 1) return ipc_unlock(&ch->rx, 0);
        if (rc == -2) break;
        if (rc == -1) {
            if (ch->server) ch->bye_blocked[chunk.client_id - 1] = 1;
            if (retire(ch, chunk.client_id) < 0) break;
            if (!ch->server) { errno = EPROTO; break; }
        }
    }
    return ipc_unlock(&ch->rx, -1);
}

const ipc_backend_t ipc_backend_sysv_mq = {
    .name = "sysv_mq", .server_open = server_open, .client_open = client_open,
    .send = send_message, .recv = receive_message, .close = mq_close, .destroy = mq_destroy
};
