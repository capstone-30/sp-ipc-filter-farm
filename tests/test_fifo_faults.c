/* Linux 기능 검증: 부분 전송 중 클라 사망, 다른 ID 진행, 서버 사망, stop.
 * 외부 timeout으로 테스트 프로세스 그룹 전체의 실행 시간을 제한한다. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"

atomic_bool g_stop = false;
static const ipc_backend_t *backend;
static const ipc_config_t cfg = { .size = 64, .depth = 1, .team = 1 };

static void nap(void)
{
    struct timespec delay = { .tv_nsec = 300000000 };
    while (nanosleep(&delay, &delay) != 0) assert(errno == EINTR);
}

static void wait_child(pid_t pid, int killed)
{
    int status;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    assert(got == pid);
    if (killed) assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    else assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void cleanup(ipc_channel_t *server, const char *key)
{
    backend->destroy(server);
    backend->close(server);
    char path[256];
    assert(snprintf(path, sizeof path, "%s.meta", key) > 0);
    assert(access(path, F_OK) == -1 && errno == ENOENT);
    for (unsigned id = 1; id <= SLOT_COUNT; id++) {
        assert(snprintf(path, sizeof path, "%s.req.%u", key, id) > 0);
        assert(access(path, F_OK) == -1 && errno == ENOENT);
        assert(snprintf(path, sizeof path, "%s.resp.%u", key, id) > 0);
        assert(access(path, F_OK) == -1 && errno == ENOENT);
    }
}

static void client_death(const char *key)
{
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t dead = fork();
    assert(dead >= 0);
    if (dead == 0) {
        assert(close(ready[0]) == 0);
        ipc_channel_t *client = backend->client_open(key, 1, &cfg);
        assert(client != NULL);
        char path[256];
        assert(snprintf(path, sizeof path, "%s.req.1", key) > 0);
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        assert(fd >= 0);
        msg_hdr_t partial = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 1 };
        assert(write(fd, &partial, 16) == 16);
        assert(write(ready[1], "x", 1) == 1);
        for (;;) pause();
    }
    assert(close(ready[1]) == 0);
    ipc_channel_t *server = backend->server_open(key, &cfg);
    assert(server != NULL);
    char token;
    assert(read(ready[0], &token, 1) == 1);
    assert(close(ready[0]) == 0);
    assert(kill(dead, SIGKILL) == 0);
    wait_child(dead, 1);

    pid_t live = fork();
    assert(live >= 0);
    if (live == 0) {
        /* fcntl 잠금은 fork로 상속되지 않는다. 상속된 fd는 먼저 닫는다. */
        backend->close(server);
        nap();
        ipc_channel_t *client = backend->client_open(key, 2, &cfg);
        assert(client != NULL);
        uint8_t px[3] = { 1, 2, 3 }, rx[3];
        msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 2,
            .seq = 42, .width = 1, .height = 1, .channels = 3,
            .payload_len = 3, .checksum = fnv1a32(px, sizeof px) };
        assert(backend->send(client, &h, px) == 0);
        assert(backend->recv(client, &h, rx, sizeof rx) == 0);
        assert(h.client_id == 2 && h.seq == 42 && h.type == MSG_RESP);
        assert(h.payload_len == sizeof rx && fnv1a32(rx, sizeof rx) == h.checksum);
        assert(memcmp(rx, px, sizeof rx) == 0);
        backend->close(client);
        _exit(0);
    }
    msg_hdr_t h;
    uint8_t rx[3];
    assert(backend->recv(server, &h, rx, sizeof rx) == 0);
    assert(h.client_id == 2 && h.seq == 42 && h.type == MSG_REQ);
    assert(h.payload_len == sizeof rx && fnv1a32(rx, sizeof rx) == h.checksum);
    h.type = MSG_RESP;
    assert(backend->send(server, &h, rx) == 0);
    wait_child(live, 0);
    h.client_id = 1;
    assert(backend->send(server, &h, rx) == -1 && errno == EPIPE);
    cleanup(server, key);
}

static void server_death(const char *key)
{
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        assert(close(ready[0]) == 0);
        assert(backend->server_open(key, &cfg) != NULL);
        assert(write(ready[1], "x", 1) == 1);
        for (;;) pause();
    }
    assert(close(ready[1]) == 0);
    char token;
    assert(read(ready[0], &token, 1) == 1);
    assert(close(ready[0]) == 0);
    ipc_channel_t *client = backend->client_open(key, 1, &cfg);
    assert(client != NULL);
    assert(kill(pid, SIGKILL) == 0);
    wait_child(pid, 1);
    msg_hdr_t h;
    uint8_t rx[3];
    assert(backend->recv(client, &h, rx, sizeof rx) == -1 && errno == ECONNRESET);
    backend->close(client);
    ipc_channel_t *server = backend->server_open(key, &cfg);
    assert(server != NULL);
    cleanup(server, key);
}

static void *stopper(void *arg)
{
    (void)arg;
    nap();
    atomic_store(&g_stop, true);
    return NULL;
}

int main(void)
{
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
    backend = ipc_lookup("fifo");
    assert(backend != NULL);
    char directory[] = "/tmp/ipcff-fault.XXXXXX", key[256];
    assert(mkdtemp(directory) != NULL);
    assert(snprintf(key, sizeof key, "%s/key", directory) > 0);
    client_death(key);
    server_death(key);
    ipc_channel_t *server = backend->server_open(key, &cfg);
    assert(server != NULL);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, stopper, NULL) == 0);
    msg_hdr_t h;
    uint8_t rx[3];
    assert(backend->recv(server, &h, rx, sizeof rx) == -1 && errno == ECANCELED);
    assert(pthread_join(thread, NULL) == 0);
    cleanup(server, key);
    assert(rmdir(directory) == 0);
    puts("test_fifo_faults: OK (클라 사망·ID 격리·서버 사망·재시작·stop·잔존 0)");
    return 0;
}
