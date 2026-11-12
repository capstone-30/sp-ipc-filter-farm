#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int clock_error;
static int test_clock_gettime(clockid_t clock, struct timespec *t)
{
    assert(clock == CLOCK_MONOTONIC);
    if (clock_error) { errno = clock_error; return -1; }
    *t = (struct timespec){ .tv_sec = 5, .tv_nsec = 123 };
    return 0;
}

/* 실제 시간을 기다리지 않고 시계 선택·단위 변환·실패 전파를 검증한다. */
#define clock_gettime test_clock_gettime
#include "../src/ipc/ipc_util.h"
#undef clock_gettime

atomic_bool g_stop = false;

/* 임시 파일·파이프로 소유 검사·부분 전송·경로 제거 경로를 확인한다. */
static void files(void)
{
    char path[] = "/tmp/ipcff_util_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    struct stat st;
    assert(ipc_stat_owned(fd, &st) == 0 && st.st_size == 0);

    uint64_t out = 0x0123456789abcdefull, in = 0;
    assert(ipc_pio(fd, &out, sizeof out, 0, 1) == 0);
    assert(ipc_pio(fd, &in, sizeof in, 0, 0) == 0 && in == out);
    /* 레코드보다 짧은 파일은 EOF에서 EIO로 끝난다. */
    uint8_t big[sizeof out * 2];
    errno = 0;
    assert(ipc_pio(fd, big, sizeof big, 0, 0) == -1 && errno == EIO);
    in = 0;
    assert(ipc_pio(fd, &out, sizeof out, 8, 1) == 0);
    assert(ipc_pio(fd, &in, sizeof in, 8, 0) == 0 && in == out);
    in = 0;
    assert(ipc_pio(fd, &in, sizeof in, 0, 0) == 0 && in == out);

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    errno = 0;
    assert(ipc_stat_owned(pipefd[0], &st) == -1 && errno == EPERM);
    assert(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    assert(close(fd) == 0);

    assert(ipc_unlink(path) == 0);
    assert(ipc_unlink(path) == 0);                 /* 이미 없으면 성공 */
    errno = 0;
    assert(ipc_unlink("/etc/hosts/nope") == -1);   /* 부모가 파일 = 실패 */
}

int main(void)
{
    uint64_t ns = 0;
    assert(ipc_now(&ns) == 0 && ns == 5000000123ull);
    clock_error = EIO;
    assert(ipc_now(&ns) == -1 && errno == EIO && ns == 5000000123ull);

    pthread_mutexattr_t attr;
    pthread_mutex_t mutex;
    assert(pthread_mutexattr_init(&attr) == 0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&mutex, &attr) == 0);
    assert(pthread_mutexattr_destroy(&attr) == 0);

    for (int rc = -1; rc <= 0; rc++) {
        assert(pthread_mutex_lock(&mutex) == 0);
        errno = EPIPE;
        assert(ipc_unlock(&mutex, rc) == rc && errno == EPIPE);
        /* 해제되었으므로 다시 획득할 수 있다. */
        assert(pthread_mutex_trylock(&mutex) == 0);
        assert(pthread_mutex_unlock(&mutex) == 0);
    }
    /* ERRORCHECK mutex의 중복 해제는 정의된 EPERM 오류다. */
    errno = EPIPE;
    assert(ipc_unlock(&mutex, 0) == -1 && errno == EPERM);
    errno = EPIPE;
    assert(ipc_unlock(&mutex, -1) == -1 && errno == EPIPE);
    assert(pthread_mutex_destroy(&mutex) == 0);

    files();

    assert(ipc_nap() == 0);
    atomic_store(&g_stop, true);
    errno = 0;
    assert(ipc_nap() == -1 && errno == ECANCELED);
    atomic_store(&g_stop, false);

    puts("test_ipc_util: OK (clock, unlock, owner stat, pio, unlink, nap)");
    return 0;
}
