#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"
#include "ipc_meta.h"
#include "ipc_util.h"

static int meta_path(char *out, size_t n, const char *key_path)
{
    int len = snprintf(out, n, "%s.meta", key_path);
    if (len < 0 || (size_t)len >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int meta_open(const char *key_path)
{
    char path[1024];
    if (meta_path(path, sizeof path, key_path) < 0) return -1;
    /* 프로세스당 fd 하나만 열어 유지한다. 다른 fd를 닫으면 잠금을 잃는다. */
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat st;
    int rc = ipc_stat_owned(fd, &st);
    if (rc == 0 && st.st_nlink != 1) { errno = EPERM; rc = -1; }
    if (rc < 0) {
        int e = errno;
        close(fd); errno = e; return -1;
    }
    return fd;
}

int meta_remove(const char *key_path)
{
    char path[1024];
    if (meta_path(path, sizeof path, key_path) < 0) return -1;
    return ipc_unlink(path);
}

int meta_trylock(int fd, uint32_t byte)
{
    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = byte, .l_len = 1};
    return fcntl(fd, F_SETLK, &fl);        /* 수명 내내 유지한다 */
}

pid_t meta_owner(int fd, uint32_t byte)
{
    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = byte, .l_len = 1};
    if (fcntl(fd, F_GETLK, &fl) == -1) return 0;
    return fl.l_type == F_UNLCK ? 0 : fl.l_pid;
}

int meta_store(int fd, const meta_t *m)
{
    return ipc_pio(fd, (void *)m, sizeof *m, META_DATA_OFF, 1);
}

int meta_load(int fd, meta_t *m)
{
    return ipc_pio(fd, m, sizeof *m, META_DATA_OFF, 0);
}

int meta_wait_ready(int fd, meta_t *m, unsigned timeout_ms)
{
    for (unsigned waited = 0; waited < timeout_ms; waited += 20) {
        if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }

        if (meta_load(fd, m) == 0 && m->magic == IPC_MAGIC && m->ready == 1) {
            errno = 0;
            pid_t owner = meta_owner(fd, META_LOCK_SERVER);
            if (!owner && errno) return -1;
            /* 재시작 전의 ready는 새 서버가 게시할 때까지 무시한다. */
            if (owner > 0 && owner == m->server_pid) return 0;
        }

        nanosleep(&(struct timespec){ .tv_nsec = 20 * 1000 * 1000 }, NULL);
    }
    errno = ETIMEDOUT;
    return -1;
}

/* used[]는 ID마다 1바이트라 클라끼리 겹치지 않는다. 구조체 전체를 읽고 쓰면
 * 서로의 갱신을 덮으므로 자기 바이트만 pread/pwrite 한다. */
int meta_claim_id(int fd, uint32_t client_id)
{
    if (client_id < 1 || client_id > SLOT_COUNT) { errno = EINVAL; return -1; }
    off_t off = META_DATA_OFF + (off_t)offsetof(meta_t, used) + (client_id - 1);
    uint8_t used = 0;

    if (ipc_pio(fd, &used, 1, off, 0) < 0) return -1;
    if (used != 0) { errno = EADDRINUSE; return -1; }   /* 서버 실행당 1회 */

    used = 1;
    return ipc_pio(fd, &used, 1, off, 1);
}

int meta_retire_id(int fd, uint32_t client_id)
{
    if (client_id < 1 || client_id > SLOT_COUNT) { errno = EINVAL; return -1; }
    off_t off = META_DATA_OFF + (off_t)offsetof(meta_t, used) + client_id - 1;
    uint8_t used = 2;
    return ipc_pio(fd, &used, 1, off, 1);
}

int meta_id_retired(int fd, uint32_t client_id)
{
    if (client_id < 1 || client_id > SLOT_COUNT) { errno = EINVAL; return -1; }
    off_t off = META_DATA_OFF + (off_t)offsetof(meta_t, used) + client_id - 1;
    uint8_t used;
    if (ipc_pio(fd, &used, 1, off, 0) < 0) return -1;
    return used == 2;
}

/* 서버는 meta를 초기화하고, 클라는 준비·설정 일치·서버 동일성 확인 후 ID를 확보한다.
 * 성공 시 *m은 서버가 게시한 값이다(클라는 m->server_pid가 곧 잠금 소유자). */
int meta_begin(int fd, uint32_t id, const char *backend, const ipc_config_t *cfg, meta_t *m)
{
    size_t len = strlen(backend) + 1;
    if (len > sizeof m->backend || !cfg->team || cfg->team > 99) { errno = EINVAL; return -1; }
    if (!id) {
        *m = (meta_t){ .magic = IPC_MAGIC, .size = cfg->size, .depth = cfg->depth,
            .team = cfg->team, .payload_bytes = cfg->payload_bytes,
            .slot_count = SLOT_COUNT, .warmup = cfg->warmup, .server_pid = (int32_t)getpid() };
        memcpy(m->backend, backend, len);
        return meta_store(fd, m);
    }
    if (meta_wait_ready(fd, m, 5000) < 0) return -1;
    if (memcmp(m->backend, backend, len) || m->size != cfg->size ||
        m->depth != cfg->depth || m->team != cfg->team || m->payload_bytes != cfg->payload_bytes ||
        m->slot_count != SLOT_COUNT || m->warmup != cfg->warmup) { errno = EINVAL; return -1; }
    errno = 0;
    pid_t owner = meta_owner(fd, META_LOCK_SERVER);
    if (!owner && errno) return -1;
    if (!owner || owner != m->server_pid) { errno = ECONNRESET; return -1; }
    return meta_claim_id(fd, id);
}
