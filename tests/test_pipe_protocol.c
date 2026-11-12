#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"

atomic_bool g_stop = false;
static int fds[2][4];

static void prepare(unsigned id)
{
    char env[256]; size_t at = 0;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        int rd = -1, wr = -1;
        if (i < 2) {
            if (!id) { rd = fds[i][0]; wr = fds[i][3]; }
            else if (i == id - 1) { rd = fds[i][2]; wr = fds[i][1]; }
            for (unsigned k = 0; k < 4; k++)
                if (fds[i][k] != rd && fds[i][k] != wr) assert(close(fds[i][k]) == 0);
        }
        int n = snprintf(env + at, sizeof env - at, "%d:%d,", rd, wr);
        assert(n > 0 && (size_t)n < sizeof env - at); at += (size_t)n;
    }
    assert(setenv("IPCFF_PIPE_FDS", env, 1) == 0);
}

static void run(int spoof)
{
    const ipc_backend_t *b = ipc_lookup("pipe"); assert(b);
    char dir[] = "/tmp/ipcff-pipe-protocol.XXXXXX", key[128];
    assert(mkdtemp(dir)); assert(snprintf(key, sizeof key, "%s/key", dir) > 0);
    ipc_config_t cfg = {.size=1, .depth=1, .team=1, .payload_bytes=64};
    for (unsigned i = 0; i < 2; i++) assert(pipe(&fds[i][0]) == 0 && pipe(&fds[i][2]) == 0);
    pid_t child[2];
    for (unsigned id = 1; id <= 2; id++) {
        child[id - 1] = fork(); assert(child[id - 1] >= 0);
        if (!child[id - 1]) {
            prepare(id);
            ipc_channel_t *c = b->client_open(key, id, &cfg); assert(c);
            uint8_t data[64]; memset(data, 42, sizeof data);
            msg_hdr_t h = {.magic=IPC_MAGIC, .type=MSG_REQ, .client_id=id,
                .seq=3, .payload_len=sizeof data, .checksum=fnv1a32(data, sizeof data)};
            if (id == 1) {
                /* ID 사칭 또는 중간 헤더에서 종료해도 ID2 스트림은 보존되어야 한다. */
                h.client_id = 2;
                size_t bytes = spoof ? sizeof h : 7;
                assert(write(fds[0][1], &h, bytes) == (ssize_t)bytes);
            } else {
                assert(b->send(c, &h, data) == 0);
                memset(data, 0, sizeof data);
                assert(b->recv(c, &h, data, sizeof data) == 0 && h.type == MSG_RESP && h.client_id == id);
                assert(fnv1a32(data, sizeof data) == h.checksum && data[0] == 42);
            }
            b->close(c); _exit(0);
        }
    }
    prepare(0);
    ipc_channel_t *s = b->server_open(key, &cfg); assert(s);
    int status;
    assert(waitpid(child[0], &status, 0) == child[0] && WIFEXITED(status) && !WEXITSTATUS(status));
    uint8_t data[64]; msg_hdr_t h;
    assert(b->recv(s, &h, data, sizeof data) == 0 && h.client_id == 2 && h.seq == 3);
    h.type = MSG_RESP;
    assert(b->send(s, &h, data) == 0);
    assert(waitpid(child[1], &status, 0) == child[1] && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(b->send(s, &h, data) == -1 && errno == EPIPE);
    atomic_store(&g_stop, true);
    assert(b->recv(s, &h, data, sizeof data) == -1 && errno == ECANCELED);
    atomic_store(&g_stop, false);
    b->destroy(s); b->close(s); assert(rmdir(dir) == 0);
}

int main(void)
{
    alarm(15);
    struct sigaction sa = {.sa_handler=SIG_IGN};
    assert(sigemptyset(&sa.sa_mask) == 0 && sigaction(SIGPIPE, &sa, NULL) == 0);
    run(0); run(1);
    puts("pipe: partial-header EOF, spoofed ID isolation, EPIPE, cancellation OK");
    return 0;
}
