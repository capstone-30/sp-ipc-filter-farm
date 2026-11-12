/* 백엔드 이름을 인자로 받아 전 백엔드가 재사용한다. 12KB / 512KB / 12MB 왕복.
 * 서버와 클라이언트는 서로 다른 프로세스여야 한다 — fcntl 잠금은 같은 프로세스
 * 안에서 서로를 막지 않으므로 단일 프로세스로는 상대 확인이 성립하지 않는다. */
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "../src/ipc/pshm_owned.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/ipc/ipc.h"

atomic_bool g_stop = false;                  /* 링크에 필요 (ipc/ipc.h) */

static const uint32_t SIDES[] = { 64, 418, 2048 };      /* 12KB / 512KB / 12MB */
#define NMSG (sizeof SIDES / sizeof SIDES[0])

static int client_main(const ipc_backend_t *b, const char *key, const ipc_config_t *cfg)
{
    ipc_channel_t *ch = b->client_open(key, 1, cfg);
    if (ch == NULL) { perror("client_open"); return 1; }

    uint8_t *tx = malloc(MAX_PAYLOAD), *rx = malloc(MAX_PAYLOAD);
    int rc = (tx == NULL || rx == NULL);

    for (size_t i = 0; i < NMSG && rc == 0; i++) {
        uint32_t s = SIDES[i], len = s * s * 3;
        for (uint32_t k = 0; k < len; k++) tx[k] = (uint8_t)(k * 7u + (uint32_t)i);

        msg_hdr_t h = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 1,
                        .seq = (uint32_t)i, .filter = F_GRAY, .width = s, .height = s,
                        .channels = 3, .payload_len = len, .checksum = fnv1a32(tx, len) };

        if (b->send(ch, &h, tx) != 0) { perror("send"); rc = 1; break; }

        msg_hdr_t rh;
        if (b->recv(ch, &rh, rx, MAX_PAYLOAD) != 0) { perror("recv"); rc = 1; break; }

        if (rh.seq != (uint32_t)i || rh.type != MSG_RESP || rh.payload_len != len ||
            fnv1a32(rx, len) != rh.checksum || memcmp(tx, rx, len) != 0) {
            fprintf(stderr, "loopback: seq %zu (%u바이트) 불일치\n", i, len);
            rc = 1;
        }
    }

    if (!rc) {
        msg_hdr_t bye = { .magic = IPC_MAGIC, .type = MSG_BYE, .client_id = 1 };
        if (b->send(ch, &bye, NULL) != 0) { perror("BYE"); rc = 1; }
    }
    free(tx); free(rx);
    b->close(ch);
    return rc;
}

static int server_main(const ipc_backend_t *b, ipc_channel_t *ch)
{
    uint8_t *buf = malloc(MAX_PAYLOAD);
    if (buf == NULL) return 1;
    int rc = 0;

    for (size_t i = 0; i < NMSG; i++) {
        msg_hdr_t h;
        if (b->recv(ch, &h, buf, MAX_PAYLOAD) != 0) { perror("srv recv"); rc = 1; break; }

        if (fnv1a32(buf, h.payload_len) != h.checksum) {   /* R2: 반드시 계산한다 */
            fprintf(stderr, "srv: 체크섬 불일치 seq=%u\n", h.seq);
            rc = 1; break;
        }

        h.type = MSG_RESP;                                 /* 그대로 되돌린다 */
        if (b->send(ch, &h, buf) != 0) { perror("srv send"); rc = 1; break; }
    }

    if (!rc) {
        msg_hdr_t bye;
        if (b->recv(ch, &bye, buf, MAX_PAYLOAD) != 0 ||
            bye.type != MSG_BYE || bye.payload_len != 0 || bye.client_id != 1) rc = 1;
    }
    free(buf);
    return rc;
}

static int leftover(const char *key)
{
    char p[512];
    struct stat st;
    int n = 0;

    const char *suffix[] = {"", ".mq", ".shm", ".pshm", ".meta"};
    for (size_t i = 0; i < sizeof suffix / sizeof suffix[0]; i++) {
        snprintf(p, sizeof p, "%s%s", key, suffix[i]);
        if (stat(p, &st) == 0) { fprintf(stderr, "잔존: %s\n", p); n++; }
    }

    for (uint32_t id = 1; id <= SLOT_COUNT; id++) {
        snprintf(p, sizeof p, "%s.req.%u", key, id);
        if (stat(p, &st) == 0) { fprintf(stderr, "잔존: %s\n", p); n++; }
        snprintf(p, sizeof p, "%s.resp.%u", key, id);
        if (stat(p, &st) == 0) { fprintf(stderr, "잔존: %s\n", p); n++; }
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <backend>\n", argv[0]); return 2; }

    const ipc_backend_t *b = ipc_lookup(argv[1]);
    if (b == NULL) { fprintf(stderr, "미지원 백엔드: %s\n", argv[1]); return 2; }

    char key[128];
    snprintf(key, sizeof key, "/tmp/ipcff.t%d", (int)getpid());
    ipc_config_t cfg = { .size = 2048, .depth = 1, .team = 1 };

    char long_key[512];
    memset(long_key, 'x', sizeof long_key - 1);
    long_key[sizeof long_key - 1] = '\0';
    errno = 0;
    if (b->server_open(long_key, &cfg) != NULL || errno != ENAMETOOLONG) return 1;
    errno = 0;
    if (b->client_open(long_key, 1, &cfg) != NULL || errno != ENAMETOOLONG) return 1;

    pid_t pid = fork();                       /* 서버 fd를 물려주지 않으려면 먼저 */
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) _exit(client_main(b, key, &cfg));

    int rc = 0;
    char name[PSHM_NAME_CAP] = {0};
    ipc_channel_t *srv = b->server_open(key, &cfg);
    if (srv == NULL) { perror("server_open"); rc = 1; }
    else {
        if (!strcmp(argv[1], "posix_shm")) {
            char path[256]; snprintf(path, sizeof path, "%s.pshm", key);
            int fd = open(path, O_RDONLY); pshm_record_t r;
            if (fd < 0) rc = 1;
            else {
                if (pshm_read(fd, &r) != 1) rc = 1;
                else memcpy(name, r.name, sizeof name);
                if (close(fd) < 0) rc = 1;
            }
        }
        if (!rc) rc = server_main(b, srv);
        b->destroy(srv);                      /* 이름·커널 객체 제거 */
        b->close(srv);                        /* fd·채널 메모리 해제 */
    }

    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = 1;
    if (leftover(key) != 0) rc = 1;
    if (*name) {
        int fd = shm_open(name, O_RDONLY, 0);
        if (fd >= 0) { rc = 1; if (close(fd) < 0) perror("close"); }
        else if (errno != ENOENT) rc = 1;
    }

    printf("test_ipc_loopback[%s]: %s (12KB/512KB/12MB 왕복, 잔존 자원 0)\n",
           argv[1], rc == 0 ? "OK" : "FAIL");
    return rc;
}
