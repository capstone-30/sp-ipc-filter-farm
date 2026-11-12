#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../src/ipc/mq_internal.h"
#include "../src/ipc/ipc_names.h"
#include "../src/ipc/ipc_util.h"

atomic_bool g_stop = false;
static const ipc_backend_t *b;
static const ipc_config_t cfg = {.size=128, .depth=2, .team=1};
static uint8_t pixels[9000];

static void sleep_ms(long ms)
{
    struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000 };
    while (nanosleep(&t, &t) < 0) assert(errno == EINTR);
}
static void child_ok(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void send_raw(int q, const mq_chunk_t *c, size_t n)
{
    while (msgsnd(q, c, n, IPC_NOWAIT) < 0) {
        assert(errno == EAGAIN || errno == EINTR); sleep_ms(1);
    }
}
static mq_chunk_t frame(uint32_t seq, uint32_t idx)
{
    msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 1, .seq = seq,
        .payload_len = sizeof pixels, .checksum = fnv1a32(pixels, sizeof pixels) };
    mq_chunk_t c = { .mtype = 1, .client_id = 1, .seq = seq, .chunk_idx = idx, .chunk_cnt = 2 };
    if (!idx) {
        c.bytes = CHUNK_PAYLOAD;
        memcpy(c.data, &h, sizeof h);
        memcpy(c.data + sizeof h, pixels, CHUNK_PAYLOAD - sizeof h);
    } else {
        c.bytes = sizeof h + sizeof pixels - CHUNK_PAYLOAD;
        memcpy(c.data, pixels + CHUNK_PAYLOAD - sizeof h, c.bytes);
    }
    return c;
}
static void bye(ipc_channel_t *ch, uint32_t id)
{
    msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_BYE, .client_id = id };
    assert(b->send(ch, &h, NULL) == 0);
}
static void cleanup(ipc_channel_t *server, const char *key)
{
    int ids[] = {server->q[0], server->q[1]};
    b->destroy(server); b->close(server);
    struct msqid_ds ds;
    for (unsigned i = 0; i < 2; i++)
        assert(msgctl(ids[i], IPC_STAT, &ds) < 0 && (errno == EINVAL || errno == EIDRM));
    char path[512];
    assert(access(key, F_OK) < 0 && errno == ENOENT);
    assert(snprintf(path, sizeof path, "%s.meta", key) > 0);
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    assert(snprintf(path, sizeof path, "%s.mq", key) > 0);
    assert(access(path, F_OK) < 0 && errno == ENOENT);
}

static void malformed(const char *key, unsigned mode)
{
    pid_t bad = fork(); assert(bad >= 0);
    if (!bad) {
        ipc_channel_t *ch = b->client_open(key, 1, &cfg); assert(ch);
        mq_chunk_t c = frame(0, 0);
        size_t n = MQ_MSGSZ(c.bytes);
        msg_hdr_t h; memcpy(&h, c.data, sizeof h);
        switch (mode) {
        case 0: send_raw(ch->q[0], &c, n); break; /* 동일 청크 중복 */
        case 1: c.chunk_idx = 2; break;
        case 2: c.chunk_cnt = 0; break;
        case 3: c.bytes = CHUNK_PAYLOAD + 1; break;
        case 4: h.client_id = 2; memcpy(c.data, &h, sizeof h); break;
        case 5: h.seq = 9; memcpy(c.data, &h, sizeof h); break;
        case 6: h.payload_len = MAX_PAYLOAD + 1; memcpy(c.data, &h, sizeof h); break;
        case 7: n--; break;
        case 8: c.chunk_cnt = UINT32_MAX; break;
        case 9:
            send_raw(ch->q[0], &c, n);
            c = frame(1, 0); send_raw(ch->q[0], &c, n);
            c = frame(2, 0); break; /* ID당 W=2 재조립 한도 */
        case 10: n = 8; break; /* ID는 읽히지만 나머지 메타데이터 부족 */
        case 11: break; /* 미완성 상태 30초 정체 */
        }
        send_raw(ch->q[0], &c, n);
        if (mode == 12) { assert(kill(getpid(), SIGKILL) == 0); _exit(1); }
        uint8_t rx[sizeof pixels];
        uint64_t start, end;
        assert(ipc_now(&start) == 0);
        assert(b->recv(ch, &h, rx, sizeof rx) == -1 && errno == ECONNRESET);
        assert(ipc_now(&end) == 0);
        if (mode == 11) assert(end - start >= 29000000000ull && end - start < 35000000000ull);
        b->close(ch); _exit(0);
    }
    ipc_channel_t *server = b->server_open(key, &cfg); assert(server);
    pid_t live = fork(); assert(live >= 0);
    if (!live) {
        b->close(server);
        sleep_ms(100);
        ipc_channel_t *ch = b->client_open(key, 2, &cfg); assert(ch);
        msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 2,
            .payload_len = 3, .checksum = fnv1a32(pixels, 3) };
        uint8_t rx[3];
        /* 정체 ID 옆에서 다른 ID가 계속 왕복해도 30초 타이머가 진행되어야 한다. */
        unsigned count = mode == 11 ? 3100 : 1;
        for (unsigned i = 0; i < count; i++) {
            h.type = MSG_REQ; h.seq = i;
            assert(b->send(ch, &h, pixels) == 0);
            assert(b->recv(ch, &h, rx, sizeof rx) == 0);
            assert(h.type == MSG_RESP && h.client_id == 2 && h.seq == i);
            assert(h.payload_len == 3 && fnv1a32(rx, 3) == h.checksum);
            if (mode == 11) sleep_ms(10);
        }
        bye(ch, 2); b->close(ch); _exit(0);
    }
    unsigned count = mode == 11 ? 3100 : 1;
    for (unsigned i = 0; i < count; i++) {
        msg_hdr_t h; uint8_t rx[sizeof pixels];
        assert(b->recv(server, &h, rx, sizeof rx) == 0);
        assert(h.client_id == 2 && h.seq == i && h.payload_len == 3);
        h.type = MSG_RESP;
        assert(b->send(server, &h, rx) == 0);
    }
    msg_hdr_t h; uint8_t rx[sizeof pixels];
    assert(b->recv(server, &h, rx, sizeof rx) == 0 && h.type == MSG_BYE);
    if (mode == 12) {
        int status;
        assert(waitpid(bad, &status, 0) == bad && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    } else child_ok(bad);
    child_ok(live);
    cleanup(server, key);
}

static void reordered(const char *key)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        ipc_channel_t *ch = b->client_open(key, 1, &cfg); assert(ch);
        const unsigned seq[] = {1, 0, 1, 0}, index[] = {1, 0, 0, 1};
        for (unsigned i = 0; i < 4; i++) {
            mq_chunk_t c = frame(seq[i], index[i]); send_raw(ch->q[0], &c, MQ_MSGSZ(c.bytes));
        }
        for (unsigned i = 0; i < 2; i++) {
            msg_hdr_t h; uint8_t rx[sizeof pixels];
            assert(b->recv(ch, &h, rx, sizeof rx) == 0 && h.seq == 1 - i);
            assert(h.payload_len == sizeof rx && fnv1a32(rx, sizeof rx) == h.checksum);
            assert(memcmp(rx, pixels, sizeof rx) == 0);
        }
        bye(ch, 1); b->close(ch); _exit(0);
    }
    ipc_channel_t *server = b->server_open(key, &cfg); assert(server);
    for (unsigned i = 0; i < 2; i++) {
        msg_hdr_t h; uint8_t rx[sizeof pixels];
        assert(b->recv(server, &h, rx, sizeof rx) == 0 && h.seq == 1 - i);
        assert(memcmp(rx, pixels, sizeof rx) == 0);
        h.type = MSG_RESP; assert(b->send(server, &h, rx) == 0);
    }
    msg_hdr_t h; uint8_t rx[sizeof pixels];
    assert(b->recv(server, &h, rx, sizeof rx) == 0 && h.type == MSG_BYE);
    child_ok(pid); cleanup(server, key);
}

static void bye_after_exit(const char *key)
{
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        ipc_channel_t *ch = b->client_open(key, 1, &cfg); assert(ch);
        msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 1,
            .payload_len = 3, .checksum = fnv1a32(pixels, 3) };
        uint8_t rx[3];
        assert(b->send(ch, &h, pixels) == 0);
        assert(b->recv(ch, &h, rx, sizeof rx) == 0);
        bye(ch, 1); b->close(ch); _exit(0);
    }
    ipc_channel_t *server = b->server_open(key, &cfg); assert(server);
    msg_hdr_t h; uint8_t rx[sizeof pixels];
    assert(b->recv(server, &h, rx, sizeof rx) == 0 && h.client_id == 1 && h.payload_len == 3);
    h.type = MSG_RESP; assert(b->send(server, &h, rx) == 0);
    /* 자식이 BYE 직후 종료해 잠금을 해제한 뒤에도 큐의 BYE를 전달해야 한다. */
    child_ok(pid);
    assert(b->recv(server, &h, rx, sizeof rx) == 0 && h.type == MSG_BYE && h.client_id == 1);
    cleanup(server, key);
}

static void restart(const char *key)
{
    int pipefd[2]; assert(pipe(pipefd) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        assert(close(pipefd[0]) == 0);
        ipc_channel_t *ch = b->server_open(key, &cfg); assert(ch);
        assert(write(pipefd[1], ch->q, sizeof ch->q) == sizeof ch->q);
        for (;;) pause();
    }
    assert(close(pipefd[1]) == 0);
    int old[2]; assert(read(pipefd[0], old, sizeof old) == sizeof old);
    assert(close(pipefd[0]) == 0);
    ipc_channel_t *ch = b->client_open(key, 1, &cfg); assert(ch);
    assert(kill(pid, SIGKILL) == 0);
    int status; assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    msg_hdr_t h; uint8_t rx[3];
    assert(b->recv(ch, &h, rx, sizeof rx) == -1 && errno == ECONNRESET);
    b->close(ch);
    ipc_channel_t *server = b->server_open(key, &cfg); assert(server);
    struct msqid_ds ds;
    for (unsigned i = 0; i < 2; i++)
        assert(msgctl(old[i], IPC_STAT, &ds) < 0 && (errno == EINVAL || errno == EIDRM));
    cleanup(server, key);
}

static void collision(const char *key)
{
    for (unsigned i = 0; i < 2; i++) {
        int fd = open(key, O_CREAT | O_EXCL | O_WRONLY, 0600);
        assert(fd >= 0 && close(fd) == 0);
        key_t keys[] = {ipc_team_key(cfg.team, 0), ipc_team_key(cfg.team, 1)};
        assert(keys[0] != (key_t)-1 && keys[1] != (key_t)-1);
        int foreign = msgget(keys[i], IPC_CREAT | IPC_EXCL | 0600);
        assert(foreign >= 0);
        assert(b->server_open(key, &cfg) == NULL && errno == EEXIST);
        struct msqid_ds ds;
        assert(msgctl(foreign, IPC_STAT, &ds) == 0);
        assert(msgget(keys[1 - i], 0) < 0 && errno == ENOENT);
        assert(msgctl(foreign, IPC_RMID, NULL) == 0);
        assert(unlink(key) == 0);
    }
    puts("test_mq: OK (소유 기록 없는 큐 보존·부분 생성 실패 회수)");
}

int main(void)
{
    b = ipc_lookup("sysv_mq"); assert(b);
    for (size_t i = 0; i < sizeof pixels; i++) pixels[i] = (uint8_t)(i * 7);
    char directory[] = "/tmp/ipcff-mq.XXXXXX", key[512];
    assert(mkdtemp(directory));
    assert(snprintf(key, sizeof key, "%s/key", directory) > 0);
    collision(key);
    reordered(key);
    bye_after_exit(key);
    for (unsigned i = 0; i < 11; i++) malformed(key, i);
    puts("test_mq: OK (교차 seq·역순 청크·잘못된 청크 11종·W 한도·ID 격리)");
    restart(key);
    malformed(key, 12);
    puts("test_mq: OK (서버 SIGKILL·기록 기반 재시작·큐/파일 잔존 0)");
    malformed(key, 11);
    puts("test_mq: OK (다른 ID 트래픽 중 실제 30초 정체 퇴역·클라 오류)");
    assert(rmdir(directory) == 0);
    return 0;
}
