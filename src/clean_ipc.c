#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ipc/ipc_meta.h"
#include "ipc/ipc_owned.h"
#include "ipc/ipc_util.h"
#include "ipc/pshm_owned.h"

atomic_bool g_stop = false;
static int records(const char *key, int kind, int *key_owned, uint32_t team)
{
    const char *suffix[] = {"mq", "shm", "pshm"};
    /* 다른 백엔드 기록이 섞여 있으면 어떤 자원도 지우지 않는다. */
    for (int i = 0; i < 3; i++) {
        if (i == kind) continue;
        char path[512]; struct stat st;
        int n = snprintf(path, sizeof path, "%s.%s", key, suffix[i]);
        if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; return -1; }
        if (lstat(path, &st) == 0) { errno = EEXIST; return -1; }
        if (errno != ENOENT) return -1;
    }
    if (kind < 0) return 0;
    char path[512];
    int n = snprintf(path, sizeof path, "%s.%s", key, suffix[kind]);
    if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; return -1; }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ipc_record_t r;
    pshm_record_t p;
    int loaded = kind == 2 ? pshm_read(fd, &p) : owned_read(fd, &r), e = errno;
    if (close(fd) < 0) return -1;
    if (loaded != 1) { errno = loaded < 0 ? e : EPROTO; return -1; }
    if (kind == 2) {
        char name[PSHM_NAME_CAP];
        if (pshm_name(team, name) < 0) return -1;
        if (strcmp(name, p.name)) { errno = EEXIST; return -1; }
        if (pshm_remove(&p) < 0) return -1;
    } else {
        if (owned_remove(kind, &r) < 0) return -1;
        *key_owned |= r.key_owned;
    }
    return ipc_unlink(path);
}
static int fifos(const char *key)
{
    char paths[SLOT_COUNT * 2][512];
    for (unsigned i = 0; i < SLOT_COUNT * 2; i++) {
        int n = snprintf(paths[i], sizeof paths[i], "%s.%s.%u", key, i % 2 ? "resp" : "req", i / 2 + 1);
        if (n < 0 || (size_t)n >= sizeof paths[i]) { errno = ENAMETOOLONG; return -1; }
        struct stat st;
        if (lstat(paths[i], &st) < 0) { if (errno == ENOENT) continue; return -1; }
        if (!S_ISFIFO(st.st_mode) || st.st_uid != geteuid()) { errno = EPERM; return -1; }
    }
    for (unsigned i = 0; i < SLOT_COUNT * 2; i++) if (ipc_unlink(paths[i]) < 0) return -1;
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 2 || !*argv[1] || strlen(argv[1]) >= 496) {
        fprintf(stderr, "usage: %s <key-path> (maximum 495 bytes)\n", argv[0]); return 1;
    }
    char path[512];
    int n = snprintf(path, sizeof path, "%s.meta", argv[1]);
    if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; perror("clean-ipc"); return 1; }
    int fd = open(path, O_RDWR | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) { puts("clean-ipc: meta 없음, 변경 없음"); return 0; }
        perror("clean-ipc"); return 1;
    }
    int e = 0;
    struct stat st;
    if (ipc_stat_owned(fd, &st) < 0) { e = errno; goto done; }
    for (unsigned id = 0; id <= SLOT_COUNT; id++)
        if (meta_trylock(fd, id) < 0) { e = errno; goto done; }
    meta_t m;
    if (meta_load(fd, &m) < 0) { e = errno ? errno : EPROTO; goto done; }
    if (m.magic != IPC_MAGIC || m.slot_count != SLOT_COUNT) { e = EPROTO; goto done; }
    const char *backends[] = {"sysv_mq", "sysv_shm", "posix_shm", "fifo", "pipe"};
    int kind = 0;
    while (kind < 5 && strncmp(m.backend, backends[kind], sizeof m.backend)) kind++;
    if (kind == 5) { e = EINVAL; goto done; }
    int is_pipe = kind == 4;
    if (kind >= 3) kind = -1;
    int key_owned = 0;
    if (records(argv[1], kind, &key_owned, m.team) < 0) { e = errno; goto done; }
    if (kind < 0 && !is_pipe && fifos(argv[1]) < 0) { e = errno; goto done; }
    if (key_owned && ipc_unlink(argv[1]) < 0) { e = errno; goto done; }
    if (ipc_unlink(path) < 0) e = errno;
done:
    if (close(fd) < 0 && !e) e = errno;
    if (e) { errno = e; perror("clean-ipc"); return 1; }
    puts("clean-ipc: 소유 자원 회수 완료");
    return 0;
}
