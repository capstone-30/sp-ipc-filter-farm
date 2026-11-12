/* 클라이언트 시작 전 ready 확인·실제 MQ 용량 기록. 측정 구간에는 실행하지 않는다. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>
#include "../src/ipc/ipc_meta.h"
#include "../src/ipc/ipc_names.h"
#include "../src/ipc/ipc_util.h"

atomic_bool g_stop = false;
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: inspect key-path\n"); return 1; }
    char path[512];
    int n = snprintf(path, sizeof path, "%s.meta", argv[1]);
    if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; goto fail; }
    for (unsigned i = 0; i < 500; i++) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            meta_t m = {0};
            int rc = ipc_pio(fd, &m, offsetof(meta_t, used), META_DATA_OFF, 0);
            int saved = errno;
            if (close(fd) < 0) goto fail;
            if (rc == 0 && m.magic == IPC_MAGIC && m.ready == 1) {
                printf("backend=%.*s team=%u size=%u bytes=%u depth=%u slots=%u warmup=%u\n",
                    (int)sizeof m.backend, m.backend, m.team, m.size, m.payload_bytes, m.depth, m.slot_count, m.warmup);
                if (!strncmp(m.backend, "sysv_mq", sizeof m.backend)) {
                    for (unsigned p = 0; p < 2; p++) {
                        int id = msgget(ipc_team_key(m.team, p), 0);
                        struct msqid_ds ds;
                        if (id < 0 || msgctl(id, IPC_STAT, &ds) < 0) goto fail;
                        printf("mq_%s_id=%d msg_qbytes=%lu\n", p ? "response" : "request", id, (unsigned long)ds.msg_qbytes);
                    }
                }
                return ferror(stdout) ? 1 : 0;
            }
            if (rc < 0 && saved != EIO) { errno = saved; goto fail; }
        } else if (errno != ENOENT) goto fail;
        struct timespec nap = {.tv_nsec = 10000000};
        if (nanosleep(&nap, NULL) < 0 && errno != EINTR) goto fail;
    }
    errno = ETIMEDOUT;
fail:
    perror("inspect"); return 1;
}
