/* 익명 pipe의 fd는 pipe_run만 생성한다. fd 출처로 ID를 검증한다. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ipc_meta.h"
#include "ipc_util.h"

typedef struct {
    msg_hdr_t h;
    size_t header, payload;
    uint8_t *buf;
} partial_t;
struct ipc_channel {
    int meta, server, rd[SLOT_COUNT], wr[SLOT_COUNT];
    uint32_t id, next;
    char key[496];
    partial_t part[SLOT_COUNT];
    atomic_bool dead[SLOT_COUNT];
    pthread_mutex_t tx, rx;
};

static void close_channel(ipc_channel_t *ch)
{
    if (!ch) return;
    int saved = errno;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        free(ch->part[i].buf);
        if (ch->rd[i] >= 0 && close(ch->rd[i]) < 0) saved = errno;
        if (ch->wr[i] >= 0 && close(ch->wr[i]) < 0) saved = errno;
    }
    if (ch->meta >= 0 && close(ch->meta) < 0) saved = errno;
    int e = pthread_mutex_destroy(&ch->tx); if (e) saved = e;
    e = pthread_mutex_destroy(&ch->rx); if (e) saved = e;
    free(ch); errno = saved;
}

static int inherited_fd(const char **p, char delimiter)
{
    char *end;
    errno = 0;
    long n = strtol(*p, &end, 10);
    if (end == *p || errno || n < -1 || n > INT_MAX || *end != delimiter) { errno = EINVAL; return -2; }
    *p = end + 1;
    if (n == -1) return -1;
    struct stat st;
    if (fstat((int)n, &st) < 0) return -2;
    if (!S_ISFIFO(st.st_mode)) { errno = EINVAL; return -2; }
    int flags = fcntl((int)n, F_GETFL);
    if (flags < 0 || fcntl((int)n, F_SETFL, flags | O_NONBLOCK) < 0) return -2;
    return (int)n;
}

static ipc_channel_t *open_channel(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    const char *p = getenv("IPCFF_PIPE_FDS");
    if (!p || !key || !ipc_config_valid(cfg) || id > SLOT_COUNT) { errno = EINVAL; return NULL; }
    if (strlen(key) >= sizeof ((ipc_channel_t *)0)->key) { errno = ENAMETOOLONG; return NULL; }
    ipc_channel_t *ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    ch->meta = -1; ch->server = !id; ch->id = id;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        ch->rd[i] = ch->wr[i] = -1; atomic_init(&ch->dead[i], false);
    }
    int e = pthread_mutex_init(&ch->tx, NULL);
    if (e) { free(ch); errno = e; return NULL; }
    e = pthread_mutex_init(&ch->rx, NULL);
    if (e) { pthread_mutex_destroy(&ch->tx); free(ch); errno = e; return NULL; }
    memcpy(ch->key, key, strlen(key) + 1);
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        ch->rd[i] = inherited_fd(&p, ':');
        if (ch->rd[i] == -2) { ch->rd[i] = -1; goto fail; }
        ch->wr[i] = inherited_fd(&p, ',');
        if (ch->wr[i] == -2) { ch->wr[i] = -1; goto fail; }
        if ((ch->rd[i] < 0) != (ch->wr[i] < 0) || (id && i != id - 1 && ch->rd[i] >= 0)) {
            errno = EINVAL; goto fail;
        }
    }
    if (*p || (id && ch->rd[id - 1] < 0)) { errno = EINVAL; goto fail; }
    ch->meta = meta_open(key);
    if (ch->meta < 0 || meta_trylock(ch->meta, id) < 0) goto fail;
    meta_t m;
    if (meta_begin(ch->meta, id, "pipe", cfg, &m) < 0) goto fail;
    if (!id) { m.ready = 1; if (meta_store(ch->meta, &m) < 0) goto fail; }
    return ch;
fail:
    e = errno; close_channel(ch); errno = e; return NULL;
}

static int send_bytes(ipc_channel_t *ch, unsigned i, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    for (size_t off = 0; off < bytes;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
        if (atomic_load(&ch->dead[i])) { errno = EPIPE; return -1; }
        ssize_t n = write(ch->wr[i], p + off, bytes - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN) return -1;
        struct pollfd pf = {.fd = ch->wr[i], .events = POLLOUT};
        int rc = poll(&pf, 1, 100);
        if (rc < 0 && errno != EINTR) return -1;
    }
    return 0;
}

static int send_message(ipc_channel_t *ch, const msg_hdr_t *h, const void *payload)
{
    if (!h || h->client_id < 1 || h->client_id > SLOT_COUNT || h->payload_len > MAX_PAYLOAD ||
        (h->payload_len && !payload) || (!ch->server && h->client_id != ch->id)) { errno = EINVAL; return -1; }
    unsigned i = h->client_id - 1;
    if (ch->wr[i] < 0) { errno = EPIPE; return -1; }
    int e = pthread_mutex_lock(&ch->tx);
    if (e) { errno = e; return -1; }
    int rc = send_bytes(ch, i, h, sizeof *h);
    if (!rc && h->payload_len) rc = send_bytes(ch, i, payload, h->payload_len);
    return ipc_unlock(&ch->tx, rc);
}

/* 1=완성, 0=부분 수신, -1=EOF/프레이밍 오류. 각 호출의 작업량을 유한하게 둔다. */
static int drain(ipc_channel_t *ch, unsigned i, msg_hdr_t *h, void *out, size_t cap)
{
    partial_t *r = &ch->part[i];
    if (r->header < sizeof r->h) {
        ssize_t n = read(ch->rd[i], (char *)&r->h + r->header, sizeof r->h - r->header);
        if (!n) { errno = ECONNRESET; return -1; }
        if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
        r->header += (size_t)n;
        if (r->header < sizeof r->h) return 0;
        if (r->h.magic != IPC_MAGIC || r->h.client_id != i + 1 ||
            r->h.payload_len > cap || r->h.payload_len > MAX_PAYLOAD) { errno = EPROTO; return -1; }
        if (r->h.payload_len && !(r->buf = malloc(r->h.payload_len))) return -1;
    }
    if (r->payload < r->h.payload_len) {
        ssize_t n = read(ch->rd[i], r->buf + r->payload, r->h.payload_len - r->payload);
        if (!n) { errno = ECONNRESET; return -1; }
        if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
        r->payload += (size_t)n;
        if (r->payload < r->h.payload_len) return 0;
    }
    *h = r->h;
    if (r->payload) memcpy(out, r->buf, r->payload);
    free(r->buf); memset(r, 0, sizeof *r);
    return 1;
}

static int receive_message(ipc_channel_t *ch, msg_hdr_t *h, void *out, size_t cap)
{
    if (!h || (cap && !out)) { errno = EINVAL; return -1; }
    int e = pthread_mutex_lock(&ch->rx);
    if (e) { errno = e; return -1; }
    while (!atomic_load(&g_stop)) {
        struct pollfd pf[SLOT_COUNT];
        unsigned map[SLOT_COUNT], n = 0;
        for (unsigned k = 0; k < SLOT_COUNT; k++) {
            unsigned i = (ch->next + k) % SLOT_COUNT;
            if (ch->rd[i] >= 0 && !atomic_load(&ch->dead[i])) {
                pf[n] = (struct pollfd){.fd = ch->rd[i], .events = POLLIN}; map[n++] = i;
            }
        }
        if (!n) { errno = ECONNRESET; return ipc_unlock(&ch->rx, -1); }
        int rc = poll(pf, n, 100);
        if (rc < 0) { if (errno == EINTR) continue; return ipc_unlock(&ch->rx, -1); }
        for (unsigned k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            unsigned i = map[k];
            int got = drain(ch, i, h, out, cap);
            if (got > 0) { ch->next = (i + 1) % SLOT_COUNT; return ipc_unlock(&ch->rx, 0); }
            if (got < 0) {
                int saved = errno;
                atomic_store(&ch->dead[i], true);
                free(ch->part[i].buf); memset(&ch->part[i], 0, sizeof ch->part[i]);
                if (!ch->server) { errno = saved; return ipc_unlock(&ch->rx, -1); }
                if (meta_retire_id(ch->meta, i + 1) < 0) return ipc_unlock(&ch->rx, -1);
            }
        }
    }
    errno = ECANCELED; return ipc_unlock(&ch->rx, -1);
}

static ipc_channel_t *server_open(const char *key, const ipc_config_t *cfg) { return open_channel(key, 0, cfg); }
static ipc_channel_t *client_open(const char *key, uint32_t id, const ipc_config_t *cfg)
{
    if (!id) { errno = EINVAL; return NULL; }
    return open_channel(key, id, cfg);
}
static void destroy(ipc_channel_t *ch)
{
    if (ch && ch->server) (void)meta_remove(ch->key);
}
const ipc_backend_t ipc_backend_pipe = {
    .name = "pipe", .server_open = server_open, .client_open = client_open,
    .send = send_message, .recv = receive_message, .close = close_channel, .destroy = destroy
};
