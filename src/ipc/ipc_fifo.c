/* 대조군 백엔드 — 설계 전문: SPEC §4.5, 수명·상대 종료: §5.5, 유한 대기: §5.4.
 * ID별 전용 FIFO로 분리해 서로 다른 클라이언트의 큰 write가 섞이지 않게 한다. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"
#include "ipc_meta.h"
#include "ipc_util.h"

#define WAIT_MS   100
#define PATHMAX   512

typedef struct {                       /* ID별 부분 수신 상태 */
    msg_hdr_t h;
    size_t    got_hdr, got_pay;
    uint8_t  *buf;
} rx_t;

struct ipc_channel {
    int      server;
    int      meta_fd;
    int      nslots;                   /* 서버 SLOT_COUNT, 클라 1 */
    uint32_t id;                       /* 클라이언트 */
    char     key[PATHMAX - 16];        /* .resp.<uint32> 및 NUL 공간 확보 */

    int  rd[SLOT_COUNT];               /* 읽기 fd */
    int  hold[SLOT_COUNT];             /* 같은 FIFO의 writer. EOF 상시 발생 방지 —
                                        * 상대 사망은 meta 잠금으로만 판정한다 */
    int  wr[SLOT_COUNT];               /* 쓰기 fd (서버 응답은 지연 open) */
    rx_t rx[SLOT_COUNT];
    atomic_bool dead[SLOT_COUNT];
    unsigned created;

    pthread_mutex_t tx, rxl;           /* 송신/수신 뮤텍스 분리 */
};

static void fifo_path(char *o, size_t n, const char *key, const char *dir, uint32_t id)
{
    snprintf(o, n, "%s.%s.%u", key, dir, id);
}

/* 1 = 준비됨, 0 = 타임아웃, -1 = 종료 요청/오류. 재시도 직전·직후 g_stop 검사. */
static int wait_io(int fd, short ev)
{
    if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }

    struct pollfd pf = { .fd = fd, .events = ev, .revents = 0 };
    int r = poll(&pf, 1, WAIT_MS);

    if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
    if (r < 0) return errno == EINTR ? 0 : -1;
    return r > 0 ? 1 : 0;
}

/* 잠금이 사라졌을 때만 종료로 판정한다. 타임아웃은 사망의 증거가 아니다. */
static int peer_alive(ipc_channel_t *ch, uint32_t idx)
{
    uint32_t byte = ch->server ? idx + 1 : META_LOCK_SERVER;
    return meta_owner(ch->meta_fd, byte) != 0;
}

static void retire(ipc_channel_t *ch, uint32_t idx)
{
    free(ch->rx[idx].buf);
    memset(&ch->rx[idx], 0, sizeof ch->rx[idx]);
    if (ch->rd[idx]   >= 0) { close(ch->rd[idx]);   ch->rd[idx]   = -1; }
    if (ch->hold[idx] >= 0) { close(ch->hold[idx]); ch->hold[idx] = -1; }
    /* wr은 송신 스레드 소유다. 여기서 닫으면 write/poll과 fd 재사용이 경합한다. */
    atomic_store(&ch->dead[idx], true);
}

static void chan_free(ipc_channel_t *ch)
{
    if (!ch) return;
    for (int i = 0; i < SLOT_COUNT; i++) {
        free(ch->rx[i].buf);
        if (ch->rd[i]   >= 0) close(ch->rd[i]);
        if (ch->hold[i] >= 0) close(ch->hold[i]);
        if (ch->wr[i]   >= 0) close(ch->wr[i]);
    }
    if (ch->meta_fd >= 0) close(ch->meta_fd);
    pthread_mutex_destroy(&ch->rxl);
    pthread_mutex_destroy(&ch->tx);
    free(ch);
}

static int open_fifo(const char *path, int mode)
{
    int fd = open(path, mode | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    int rc = fstat(fd, &st);
    if (!rc && (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid())) { errno = EPERM; rc = -1; }
    if (rc < 0) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

/* 읽기 fd + 같은 FIFO의 writer를 함께 연다(EOF 방지). 순서를 바꾸면 ENXIO. */
static int open_read_side(ipc_channel_t *ch, uint32_t idx, const char *path)
{
    ch->rd[idx] = open_fifo(path, O_RDONLY);
    if (ch->rd[idx] < 0) return -1;

    ch->hold[idx] = open_fifo(path, O_WRONLY);
    return ch->hold[idx] < 0 ? -1 : 0;
}

static int create_fifo(ipc_channel_t *ch, const char *path, unsigned bit)
{
    if (mkfifo(path, 0600) == 0) { ch->created |= 1u << bit; return 0; }
    if (errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid()) { errno = EPERM; return -1; }
    return 0;
}

static ipc_channel_t *open_channel(const char *key_path, uint32_t id, const ipc_config_t *cfg)
{
    if (!key_path || !ipc_config_valid(cfg) || id > SLOT_COUNT) { errno = EINVAL; return NULL; }

    if (strlen(key_path) >= sizeof ((ipc_channel_t *)0)->key) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    ipc_channel_t *ch = calloc(1, sizeof *ch);
    if (ch == NULL) return NULL;

    ch->meta_fd = -1;
    for (int i = 0; i < SLOT_COUNT; i++) {
        ch->rd[i] = ch->hold[i] = ch->wr[i] = -1;
        atomic_init(&ch->dead[i], false);
    }
    snprintf(ch->key, sizeof ch->key, "%s", key_path);

    if (pthread_mutex_init(&ch->tx, NULL) != 0)  { free(ch); return NULL; }
    if (pthread_mutex_init(&ch->rxl, NULL) != 0) { pthread_mutex_destroy(&ch->tx); free(ch); return NULL; }
    ch->server = id == 0;
    ch->nslots = ch->server ? SLOT_COUNT : 1;
    ch->id = id;

    int own_meta = 0;
    ch->meta_fd = meta_open(key_path);
    if (ch->meta_fd < 0) goto fail;
    if (meta_trylock(ch->meta_fd, id) == -1) { errno = EADDRINUSE; goto fail; }
    own_meta = ch->server;

    /* ready=0으로 초기화 → IPC 생성 → ready=1. used[]는 서버 기동 시에만 0. */
    meta_t m;
    if (meta_begin(ch->meta_fd, id, "fifo", cfg, &m) < 0) goto fail;

    for (uint32_t slot = 1; slot <= (ch->server ? SLOT_COUNT : 1); slot++) {
        char req[PATHMAX], resp[PATHMAX];
        uint32_t client_id = ch->server ? slot : id;
        fifo_path(req,  sizeof req,  key_path, "req",  client_id);
        fifo_path(resp, sizeof resp, key_path, "resp", client_id);
        if (ch->server) {
            if (create_fifo(ch, req, (slot - 1) * 2) < 0 ||
                create_fifo(ch, resp, (slot - 1) * 2 + 1) < 0) goto fail;
            if (open_read_side(ch, slot - 1, req) != 0) goto fail;
            /* 응답 쓰기 fd는 클라가 리더를 열 때까지 첫 send에서 지연 open한다. */
        } else {
            if (open_read_side(ch, 0, resp) != 0) goto fail;
            ch->wr[0] = open_fifo(req, O_WRONLY);
            if (ch->wr[0] < 0) goto fail;
        }
    }

    if (ch->server) {
        m.ready = 1;
        if (meta_store(ch->meta_fd, &m) != 0) goto fail;
    }
    return ch;
fail:
    {
        int e = errno;
        for (unsigned i = 0; i < SLOT_COUNT * 2; i++) if (ch->created & (1u << i)) {
            char path[PATHMAX];
            fifo_path(path, sizeof path, ch->key, i % 2 ? "resp" : "req", i / 2 + 1);
            unlink(path);
        }
        if (own_meta) meta_remove(ch->key);
        chan_free(ch); errno = e;
    }
    return NULL;
}

static ipc_channel_t *fifo_server_open(const char *key_path, const ipc_config_t *cfg)
{
    return open_channel(key_path, 0, cfg);
}

static ipc_channel_t *fifo_client_open(const char *key_path, uint32_t client_id,
                                       const ipc_config_t *cfg)
{
    if (!client_id) { errno = EINVAL; return NULL; }
    return open_channel(key_path, client_id, cfg);
}

static int ensure_wr(ipc_channel_t *ch, uint32_t idx)
{
    if (ch->wr[idx] >= 0) return 0;

    char path[PATHMAX];
    fifo_path(path, sizeof path, ch->key, "resp", idx + 1);

    for (;;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }

        int fd = open_fifo(path, O_WRONLY);
        if (fd >= 0) { ch->wr[idx] = fd; return 0; }
        if (errno != ENXIO) return -1;                  /* 리더 부재만 재시도 */
        if (!peer_alive(ch, idx)) { errno = EPIPE; return -1; }
        if (ipc_nap() < 0) return -1;
    }
}

static int write_all(ipc_channel_t *ch, uint32_t idx, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    for (size_t off = 0; off < n; ) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
        if (atomic_load(&ch->dead[idx])) { errno = EPIPE; return -1; }

        ssize_t w = write(ch->wr[idx], p + off, n - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w == -1 && errno == EINTR) continue;
        if (w == -1 && errno != EAGAIN) return -1;      /* EPIPE 포함 */

        int r = wait_io(ch->wr[idx], POLLOUT);
        if (r < 0) return -1;
        if (r == 0 && !peer_alive(ch, idx)) { errno = EPIPE; return -1; }
    }
    return 0;
}

static int fifo_send(ipc_channel_t *ch, const msg_hdr_t *h, const void *payload)
{
    if (!h || h->payload_len > MAX_PAYLOAD || (h->payload_len && !payload) ||
        h->client_id < 1 || h->client_id > SLOT_COUNT ||
        (!ch->server && h->client_id != ch->id)) { errno = EINVAL; return -1; }

    uint32_t idx = ch->server ? h->client_id - 1 : 0;
    if (ch->dead[idx]) { errno = EPIPE; return -1; }

    int e = pthread_mutex_lock(&ch->tx);
    if (e) { errno = e; return -1; }
    int rc = ensure_wr(ch, idx);
    if (rc == 0) rc = write_all(ch, idx, h, sizeof *h);
    if (rc == 0 && h->payload_len > 0) rc = write_all(ch, idx, payload, h->payload_len);
    return ipc_unlock(&ch->tx, rc);
}

/* 1 = 메시지 완성, 0 = 아직, -1 = 프레이밍 오류(해당 ID 퇴역). */
static int drain(ipc_channel_t *ch, uint32_t idx, msg_hdr_t *h, void *payload, size_t cap)
{
    rx_t *r = &ch->rx[idx];
    int fd = ch->rd[idx];

    while (r->got_hdr < sizeof r->h) {
        ssize_t n = read(fd, (uint8_t *)&r->h + r->got_hdr, sizeof r->h - r->got_hdr);
        if (n > 0) { r->got_hdr += (size_t)n; continue; }
        if (n == -1 && errno == EINTR) continue;
        return 0;                                       /* EAGAIN 또는 데이터 없음 */
    }

    /* 복사·할당 전에 검사한다 (SPEC §3). ID는 헤더를 믿지 않고 어느 FIFO로
     * 들어왔는지로 판정한다 — 그러지 않으면 클라가 남의 ID를 사칭할 수 있다. */
    uint32_t want = ch->server ? idx + 1 : ch->id;
    if (r->h.magic != IPC_MAGIC || r->h.payload_len > MAX_PAYLOAD ||
        r->h.payload_len > cap || r->h.client_id != want) {
        errno = EPROTO;
        return -1;
    }
    if (r->buf == NULL && r->h.payload_len > 0) {
        r->buf = malloc(r->h.payload_len);
        if (r->buf == NULL) return -1;
    }

    while (r->got_pay < r->h.payload_len) {
        ssize_t n = read(fd, r->buf + r->got_pay, r->h.payload_len - r->got_pay);
        if (n > 0) { r->got_pay += (size_t)n; continue; }
        if (n == -1 && errno == EINTR) continue;
        return 0;
    }

    *h = r->h;
    if (r->h.payload_len > 0) memcpy(payload, r->buf, r->h.payload_len);

    free(r->buf);
    memset(r, 0, sizeof *r);
    return 1;
}

static int fifo_recv(ipc_channel_t *ch, msg_hdr_t *h, void *payload, size_t cap)
{
    if (!h || (cap && !payload)) { errno = EINVAL; return -1; }
    int e = pthread_mutex_lock(&ch->rxl);
    if (e) { errno = e; return -1; }

    for (;;) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; break; }

        struct pollfd pf[SLOT_COUNT];
        uint32_t map[SLOT_COUNT];
        nfds_t n = 0;

        for (int i = 0; i < ch->nslots; i++)
            if (ch->rd[i] >= 0 && !ch->dead[i]) {
                pf[n] = (struct pollfd){ .fd = ch->rd[i], .events = POLLIN };
                map[n++] = (uint32_t)i;
            }
        if (n == 0) { errno = ECONNRESET; break; }       /* 살아 있는 상대 없음 */

        int r = poll(pf, n, WAIT_MS);
        if (r < 0) { if (errno == EINTR) continue; break; }

        for (nfds_t k = 0; k < n; k++) {
            if (!(pf[k].revents & POLLIN)) continue;

            int got = drain(ch, map[k], h, payload, cap);
            if (got == 1) return ipc_unlock(&ch->rxl, 0);
            if (got < 0) retire(ch, map[k]);        /* 프레이밍 실패 = 퇴역 */
        }

        if (r == 0) {
            /* 100ms 대기가 지났다. 상대가 필요한 쪽만 잠금을 확인한다. */
            if (!ch->server) {
                if (!peer_alive(ch, 0)) { errno = ECONNRESET; break; }
            } else {
                for (int i = 0; i < ch->nslots; i++)
                    if (!ch->dead[i] && (ch->rx[i].got_hdr > 0 || ch->rx[i].got_pay > 0) &&
                        !peer_alive(ch, (uint32_t)i))
                        retire(ch, (uint32_t)i);         /* 미완성 버퍼 폐기 */
            }
        }
    }

    return ipc_unlock(&ch->rxl, -1);
}

static void fifo_destroy(ipc_channel_t *ch)                /* 서버 전용 */
{
    if (ch == NULL || !ch->server) return;

    for (uint32_t id = 1; id <= SLOT_COUNT; id++) {
        char p[PATHMAX];
        fifo_path(p, sizeof p, ch->key, "req",  id); unlink(p);
        fifo_path(p, sizeof p, ch->key, "resp", id); unlink(p);
    }
    meta_remove(ch->key);
}

const ipc_backend_t ipc_backend_fifo = {
    .name        = "fifo",
    .server_open = fifo_server_open,
    .client_open = fifo_client_open,
    .send        = fifo_send,
    .recv        = fifo_recv,
    .close       = chan_free,
    .destroy     = fifo_destroy,
};
