#ifndef IPC_UTIL_H
#define IPC_UTIL_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"

static inline int ipc_now(uint64_t *ns)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    *ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    return 0;
}

/* 원래 작업 오류를 우선하며, 성공한 작업에는 잠금 해제 오류를 전파한다. */
static inline int ipc_unlock(pthread_mutex_t *mutex, int rc)
{
    int saved = errno, e = pthread_mutex_unlock(mutex);
    if (e && rc == 0) { errno = e; return -1; }
    errno = saved;
    return rc;
}

/* 이미 없는 경로는 성공으로 본다. */
static inline int ipc_unlink(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

/* 소유 기록·meta는 호출자 소유의 정규 파일이어야 한다. */
static inline int ipc_stat_owned(int fd, struct stat *st)
{
    if (fstat(fd, st) < 0) return -1;
    if (!S_ISREG(st->st_mode) || st->st_uid != geteuid()) { errno = EPERM; return -1; }
    return 0;
}

/* 고정 길이 레코드를 부분 전송·EINTR에 관계없이 끝까지 읽거나 쓴다. */
static inline int ipc_pio(int fd, void *buf, size_t len, off_t offset, int writing)
{
    for (size_t off = 0; off < len; ) {
        ssize_t n = writing ? pwrite(fd, (char *)buf + off, len - off, offset + (off_t)off)
                            : pread(fd, (char *)buf + off, len - off, offset + (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        off += (size_t)n;
    }
    return 0;
}

/* 재시도 사이 1ms 양보. 대기 전후로 종료 요청을 확인한다. */
static inline int ipc_nap(void)
{
    struct timespec t = { .tv_nsec = 1000000 };
    if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
    while (nanosleep(&t, &t) < 0) {
        if (errno != EINTR) return -1;
        if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
    }
    if (atomic_load(&g_stop)) { errno = ECANCELED; return -1; }
    return 0;
}
#endif
