#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/common/app.h"
#include "../src/ipc/ipc_names.h"
#include "../src/ipc/ipc_meta.h"
#include "../src/ipc/pshm_owned.h"

atomic_bool g_stop = false;

static void options(void)
{
    options_t o;
    char *args[] = {"server", "--ipc=fifo", "--team=99", "--team=2"};
    assert(app_options(2, args, 1, &o) == 0 && o.ipc.team == 1);
    assert(app_options(3, args, 1, &o) == 0 && o.ipc.team == 99);
    assert(app_options(4, args, 1, &o) == -1 && errno == EINVAL);
    const char *bad[] = {"--team=0", "--team=100", "--team=-1", "--team=",
        "--team=1x", "--team=9999999999999999999999999999"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        args[2] = (char *)bad[i];
        assert(app_options(3, args, 1, &o) == -1 && errno == EINVAL);
    }
    char *client[] = {"client", "--ipc=fifo", "--team=2", "--id=1",
        "--filter=gray", "--count=1"};
    assert(app_options(6, client, 0, &o) == 0 && o.ipc.team == 2);
    char name[PSHM_NAME_CAP];
    for (uint32_t team = 1; team <= 99; team++) {
        for (unsigned purpose = 0; purpose < 4; purpose++)
            assert(ipc_team_key(team, purpose) == (key_t)(60000 + team * 10 + purpose));
        char expected[PSHM_NAME_CAP];
        assert(snprintf(expected, sizeof expected, "/osproject_%u_shm", team) > 0);
        assert(pshm_name(team, name) == 0 && !strcmp(name, expected));
    }
    assert(ipc_team_key(0, 0) == (key_t)-1 && errno == EINVAL);
    assert(ipc_team_key(100, 0) == (key_t)-1 && errno == EINVAL);
    assert(ipc_team_key(1, 4) == (key_t)-1 && errno == EINVAL);
    assert(pshm_name(0, name) == -1 && errno == EINVAL);
    assert(pshm_name(100, name) == -1 && errno == EINVAL);
}

static void exchange(const ipc_backend_t *b, ipc_channel_t *ch)
{
    uint8_t byte = 0;
    msg_hdr_t h;
    assert(b->recv(ch, &h, &byte, 1) == 0 && byte == 42);
    assert(h.type == MSG_REQ && h.client_id == 1 && h.seq == 7);
    h.type = MSG_RESP;
    assert(b->send(ch, &h, &byte) == 0);
}

static void records(void)
{
    char path[] = "/tmp/ipcff-team-record.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    pshm_record_t r = {.magic=IPC_MAGIC, .inode=1}, loaded;
    assert(pshm_name(99, r.name) == 0 && pshm_write(fd, &r) == 0);
    assert(pshm_read(fd, &loaded) == 1 && !strcmp(loaded.name, r.name));
    const char *bad[] = {"/osproject_0_shm", "/osproject_100_shm", "/osproject_01_shm",
        "/osproject_+1_shm", "/osproject_1_shm/other", "/osproject_1_shm.extra", "/other"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(r.name, 0, sizeof r.name);
        assert(snprintf(r.name, sizeof r.name, "%s", bad[i]) > 0);
        assert(pshm_write(fd, &r) == 0);
        assert(pshm_read(fd, &loaded) == -1 && errno == EPROTO);
    }
    memset(r.name, 'x', sizeof r.name);
    assert(pshm_write(fd, &r) == 0);
    assert(pshm_read(fd, &loaded) == -1 && errno == EPROTO);
    assert(close(fd) == 0 && unlink(path) == 0);
}

static void metadata(void)
{
    char path[] = "/tmp/ipcff-team-meta.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    meta_t m, loaded;
    assert(meta_load(fd, &loaded) == -1 && errno == EIO);
    assert(pwrite(fd, "keep", 4, 0) == 4);
    ipc_config_t cfg = {.size=1, .depth=1, .team=1};
    assert(meta_begin(fd, 0, "fifo", &cfg, &m) == 0);
    assert(meta_load(fd, &loaded) == 0 && !memcmp(&m, &loaded, sizeof m));
    assert(meta_claim_id(fd, 1) == 0 && meta_claim_id(fd, 2) == 0);
    assert(meta_claim_id(fd, 1) == -1 && errno == EADDRINUSE);
    assert(meta_id_retired(fd, 1) == 0 && meta_retire_id(fd, 1) == 0);
    assert(meta_id_retired(fd, 1) == 1 && meta_id_retired(fd, 2) == 0);
    assert(meta_load(fd, &loaded) == 0 && loaded.used[0] == 2 && loaded.used[1] == 1);
    assert(meta_claim_id(fd, 0) == -1 && errno == EINVAL);
    assert(meta_retire_id(fd, SLOT_COUNT + 1) == -1 && errno == EINVAL);
    assert(meta_id_retired(fd, 0) == -1 && errno == EINVAL);
    char prefix[4];
    assert(pread(fd, prefix, sizeof prefix, 0) == 4 && !memcmp(prefix, "keep", 4));
    assert(close(fd) == 0 && unlink(path) == 0);
}

static void startup_metadata(void)
{
    char path[] = "/tmp/ipcff-startup-meta.XXXXXX";
    int fd = mkstemp(path), command[2], reply[2];
    assert(fd >= 0 && pipe(command) == 0 && pipe(reply) == 0);
    ipc_config_t cfg = {.size=1, .depth=1, .team=1};
    meta_t m, loaded;
    assert(meta_begin(fd, 0, "fifo", &cfg, &m) == 0);
    m.ready = 1; m.size = 2; m.used[0] = 1;
    assert(meta_store(fd, &m) == 0);
    /* 이전 ready가 남았어도 서버 잠금이 없으면 준비 완료가 아니다. */
    assert(meta_wait_ready(fd, &loaded, 40) == -1 && errno == ETIMEDOUT);

    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        assert(close(command[1]) == 0 && close(reply[0]) == 0);
        char byte = 0;
        assert(meta_trylock(fd, META_LOCK_SERVER) == 0);
        assert(write(reply[1], &byte, 1) == 1);
        assert(read(command[0], &byte, 1) == 1);
        assert(meta_begin(fd, 0, "fifo", &cfg, &m) == 0);
        assert(write(reply[1], &byte, 1) == 1);
        assert(read(command[0], &byte, 1) == 1);
        m.ready = 1;
        assert(meta_store(fd, &m) == 0);
        assert(write(reply[1], &byte, 1) == 1);
        assert(read(command[0], &byte, 1) == 1);
        assert(close(fd) == 0);
        _exit(0);
    }
    assert(close(command[0]) == 0 && close(reply[1]) == 0);
    char byte;
    assert(read(reply[0], &byte, 1) == 1);
    /* 새 서버가 잠금을 얻었지만 이전 PID의 ready가 남은 구간이다. */
    assert(meta_wait_ready(fd, &loaded, 40) == -1 && errno == ETIMEDOUT);
    assert(write(command[1], &byte, 1) == 1 && read(reply[0], &byte, 1) == 1);
    assert(meta_wait_ready(fd, &loaded, 40) == -1 && errno == ETIMEDOUT);
    assert(write(command[1], &byte, 1) == 1 && read(reply[0], &byte, 1) == 1);
    assert(meta_trylock(fd, 1) == 0);
    assert(meta_begin(fd, 1, "fifo", &cfg, &loaded) == 0 && loaded.server_pid == pid);
    assert(meta_load(fd, &loaded) == 0 && loaded.used[0] == 1);
    assert(write(command[1], &byte, 1) == 1);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    atomic_store(&g_stop, true);
    assert(meta_wait_ready(fd, &loaded, 40) == -1 && errno == ECANCELED);
    atomic_store(&g_stop, false);
    assert(close(command[1]) == 0 && close(reply[0]) == 0);
    assert(close(fd) == 0 && unlink(path) == 0);
    puts("Startup: stale ready without owner/with new PID and ready=0 rejected; new server claims ID; cancellation OK");
}

static void channels(const char *backend)
{
    const ipc_backend_t *b = ipc_lookup(backend);
    assert(b);
    char directory[] = "/tmp/ipcff-team.XXXXXX", key[128], other[128];
    assert(mkdtemp(directory));
    assert(snprintf(key, sizeof key, "%s/one", directory) > 0);
    assert(snprintf(other, sizeof other, "%s/two", directory) > 0);
    ipc_config_t cfg = {.size=1, .depth=1, .team=1}, second = cfg;
    ipc_config_t invalid[] = {
        {.size=0, .depth=1, .team=1}, {.size=2049, .depth=1, .team=1},
        {.size=1, .depth=0, .team=1}, {.size=1, .depth=17, .team=1},
        {.size=1, .depth=1, .team=0}, {.size=1, .depth=1, .team=100}
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert(!b->server_open(key, &invalid[i]) && errno == EINVAL);
        assert(!b->client_open(key, 1, &invalid[i]) && errno == EINVAL);
    }
    ipc_channel_t *s = b->server_open(key, &cfg);
    assert(s);
    /* FIFO는 경로별 독립성 유지. 숫자 key/이름 백엔드만 충돌한다. */
    if (strcmp(backend, "fifo"))
        assert(!b->server_open(other, &cfg) && errno == EEXIST);
    second.team = 2;
    ipc_channel_t *t = b->server_open(other, &second);
    assert(t);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        b->close(s); b->close(t);
        assert(!b->client_open(key, 1, &second) && errno == EINVAL);
        ipc_config_t wrong = cfg;
        wrong.warmup = 1;
        assert(!b->client_open(key, 1, &wrong) && errno == EINVAL);
        wrong = cfg; wrong.payload_bytes = 64;
        assert(!b->client_open(key, 1, &wrong) && errno == EINVAL);
        ipc_channel_t *c = b->client_open(key, 1, &cfg);
        assert(c); /* 불일치가 ID를 소비하지 않았는지도 확인한다. */
        uint8_t byte = 42;
        msg_hdr_t h = {.magic=IPC_MAGIC, .type=MSG_REQ, .client_id=1, .seq=7,
            .payload_len=1, .checksum=fnv1a32(&byte, 1)};
        assert(b->send(c, &h, &byte) == 0);
        assert(b->recv(c, &h, &byte, 1) == 0 && h.type == MSG_RESP && byte == 42);
        b->close(c);
        c = b->client_open(other, 1, &second);
        assert(c); h.type = MSG_REQ;
        assert(b->send(c, &h, &byte) == 0);
        assert(b->recv(c, &h, &byte, 1) == 0 && h.type == MSG_RESP && byte == 42);
        b->close(c); _exit(0);
    }
    exchange(b, s); exchange(b, t);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    b->destroy(t); b->close(t); b->destroy(s); b->close(s);
    assert(rmdir(directory) == 0);
    printf("Team: %s mismatch before ID claim, two-team isolation, cleanup OK\n", backend);
}

int main(void)
{
    alarm(20);
    options(); records(); metadata(); startup_metadata();
    const char *backends[] = {"fifo", "sysv_mq", "sysv_shm", "posix_shm"};
    for (size_t i = 0; i < sizeof backends / sizeof backends[0]; i++) channels(backends[i]);
    return 0;
}
