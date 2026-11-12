/* 실제 클라이언트에 역순·중복·미송신 seq·변조 시각 응답을 보낸다. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"

atomic_bool g_stop = false;

static void responses(const ipc_backend_t *b, unsigned mode)
{
    char directory[] = "/tmp/ipcff-client.XXXXXX", key[128], option[160], backend[64];
    assert(mkdtemp(directory));
    assert(snprintf(key, sizeof key, "%s/key", directory) > 0);
    assert(snprintf(option, sizeof option, "--key=%s", key) > 0);
    assert(snprintf(backend, sizeof backend, "--ipc=%s", b->name) > 0);
    ipc_config_t cfg = {.size=1, .depth=2, .team=1};
    ipc_channel_t *s = b->server_open(key, &cfg);
    assert(s);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        b->close(s);
        int fd = open("/dev/null", O_WRONLY);
        assert(fd >= 0 && dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0);
        assert(close(fd) == 0);
        execl("./client", "client", backend, option, "--size=1", "--depth=2",
            "--id=1", "--filter=gray", "--count=4", "--no-save", (char *)NULL);
        _exit(127);
    }
    for (unsigned batch = 0; batch < (mode ? 1u : 2u); batch++) {
        msg_hdr_t h[2];
        uint8_t data[2][3];
        for (unsigned i = 0; i < 2; i++) {
            assert(b->recv(s, &h[i], data[i], sizeof data[i]) == 0);
            assert(h[i].type == MSG_REQ && h[i].seq == batch * 2 + i);
            h[i].type = MSG_RESP;
        }
        if (mode == 2) h[1].seq = 2; /* W=2: seq 2를 아직 송신할 수 없다. */
        if (mode == 3) h[1].t_send_ns++;
        assert(b->send(s, &h[1], data[1]) == 0);
        if (mode == 1) assert(b->send(s, &h[1], data[1]) == 0);
        if (!mode) assert(b->send(s, &h[0], data[0]) == 0);
    }
    if (!mode) {
        msg_hdr_t bye;
        assert(b->recv(s, &bye, NULL, 0) == 0 && bye.type == MSG_BYE);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    assert(WEXITSTATUS(status) == (mode ? 1 : 0));
    b->destroy(s); b->close(s);
    assert(rmdir(directory) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    alarm(20);
    const ipc_backend_t *b = ipc_lookup(argv[1]);
    assert(b);
    for (unsigned mode = 0; mode < 4; mode++) responses(b, mode);
    printf("Client: %s reverse responses accepted; duplicate, unsent seq, timestamp rejected OK\n", b->name);
    return 0;
}
