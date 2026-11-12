#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "../ipc/ipc_util.h"
#ifndef BASIC_BENCH
#define BASIC_BENCH 0
#endif

static int number(const char *s, uint32_t max, uint32_t *out)
{
    if (*s == '\0') return -1;
    uint64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        n = n * 10 + (unsigned)(*s - '0');
        if (n > max) return -1;
    }
    if (n == 0) return -1;
    *out = (uint32_t)n;
    return 0;
}

int app_options(int argc, char **argv, int server, options_t *o)
{
    *o = (options_t){ .key = "/tmp/ipcff.key",
        .ipc = {.size = 512, .depth = 1, .team = 1}, .workers = 2 };
    if (BASIC_BENCH) { o->ipc.payload_bytes = 64; o->no_save = 1; }
    const struct {
        const char *name;
        uint32_t *value, min, max;
        unsigned bit;
        int enabled;
    } numbers[] = {
        {"--team=", &o->ipc.team, 1, 99, 2048u, 1},
        {"--size=", &o->ipc.size, 1, 2048, 4u, 1},
        {"--depth=", &o->ipc.depth, 1, 16, 8u, 1},
        {"--count=", &o->count, 1, UINT32_MAX, 16u, 1},
        {"--workers=", &o->workers, 1, UINT32_MAX, 32u, server},
        {"--clients=", &o->clients, 1, SLOT_COUNT, 64u, server},
        {"--id=", &o->id, 1, SLOT_COUNT, 128u, !server},
        {"--warmup=", &o->warmup, 0, UINT32_MAX, 4096u, 1},
        {"--bytes=", &o->ipc.payload_bytes, 64, 1048576, 32768u, BASIC_BENCH}
    };
    unsigned seen = 0;
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        unsigned bit = 0;
        int rc = 0;
        if (strcmp(arg, "--help") == 0 && argc == 2) {
            printf("usage: %s --ipc=<name> [--team=1] [--size=512] [--depth=1] [--key=path] %s\n",
                argv[0], server ? "[--workers=2] [--clients=N] [--count=N]" :
                "--id=N --filter=gray|blur|edge --count=N [--no-save] [--csv=path]");
            puts("measurement: --warmup=N --quiet; server: --cpu-csv=path --inplace-gray");
            if (BASIC_BENCH) puts("basic echo: --bytes=64..1048576; --filter optional; IDs 1..8");
            return 1;
        }
        for (size_t j = 0; j < sizeof numbers / sizeof numbers[0]; j++) {
            size_t len = strlen(numbers[j].name);
            if (!numbers[j].enabled || strncmp(arg, numbers[j].name, len) != 0) continue;
            bit = numbers[j].bit;
            const char *value = arg + len;
            if (!numbers[j].min && strcmp(value, "0") == 0) *numbers[j].value = 0;
            else rc = number(value, numbers[j].max, numbers[j].value);
            if (!rc && *numbers[j].value < numbers[j].min) rc = -1;
            break;
        }
        if (!bit) {
            if (strncmp(arg, "--ipc=", 6) == 0) {
                bit = 1u; o->backend = ipc_lookup(arg + 6); rc = o->backend ? 0 : -1;
            } else if (strncmp(arg, "--key=", 6) == 0) {
                bit = 2u; o->key = arg + 6; rc = *o->key ? 0 : -1;
            } else if (!server && strncmp(arg, "--filter=", 9) == 0) {
                bit = 256u;
                if (strcmp(arg + 9, "gray") == 0) o->filter = F_GRAY;
                else if (strcmp(arg + 9, "blur") == 0) o->filter = F_BLUR;
                else if (strcmp(arg + 9, "edge") == 0) o->filter = F_EDGE;
                else rc = -1;
            } else if (!server && strcmp(arg, "--no-save") == 0) {
                bit = 512u; o->no_save = 1;
            } else if (!server && strncmp(arg, "--csv=", 6) == 0) {
                bit = 1024u; o->csv = arg + 6; rc = *o->csv ? 0 : -1;
            } else if (strcmp(arg, "--quiet") == 0) {
                bit = 8192u; o->quiet = 1;
            } else if (server && strncmp(arg, "--cpu-csv=", 10) == 0) {
                bit = 16384u; o->cpu_csv = arg + 10; rc = *o->cpu_csv ? 0 : -1;
            } else if (server && !BASIC_BENCH && strcmp(arg, "--inplace-gray") == 0) {
                bit = 65536u; o->inplace_gray = 1;
            } else rc = -1;
        }
        if (rc != 0 || (seen & bit)) { errno = EINVAL; return -1; }
        seen |= bit;
    }
    unsigned required = 16u | 128u | (BASIC_BENCH ? 0u : 256u);
    if (!o->backend || (!server && (seen & required) != required) || (BASIC_BENCH && o->filter != F_GRAY) ||
        (uint64_t)o->count + o->warmup > UINT32_MAX ||
        (o->cpu_csv && (!o->warmup || !o->clients || !o->count))) {
        errno = EINVAL; return -1;
    }
    o->ipc.warmup = o->warmup;
    return 0;
}

int app_valid_payload(const msg_hdr_t *h, uint32_t type, const options_t *o)
{
    return h->magic == IPC_MAGIC && h->type == type && h->client_id >= 1 &&
        h->client_id <= SLOT_COUNT && h->width == o->ipc.size && h->height == o->ipc.size &&
        h->channels == 3 && h->filter <= (o->ipc.payload_bytes ? F_GRAY : F_EDGE) &&
        h->payload_len == ipc_payload_size(&o->ipc);
}

int app_signals(sigset_t *set)
{
    if (sigemptyset(set) == -1 || sigaddset(set, SIGINT) == -1) return -1;
    int e = pthread_sigmask(SIG_BLOCK, set, NULL);
    if (e) { errno = e; return -1; }
    struct sigaction ignore = { .sa_handler = SIG_IGN };
    if (sigemptyset(&ignore.sa_mask) == -1) return -1;
    return sigaction(SIGPIPE, &ignore, NULL);
}

int app_tick(const sigset_t *set)
{
    int rc = sigtimedwait(set, NULL, &(struct timespec){ .tv_nsec = 100000000 });
    if (rc == SIGINT) return 1;
    if (rc < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;
}

int app_acquire(sem_t *sem)
{
    while (!atomic_load(&g_stop)) {
        if (sem_trywait(sem) == 0) return 0;
        if (errno != EAGAIN && errno != EINTR) return -1;
        if (ipc_nap() != 0) return -1;
    }
    errno = ECANCELED;
    return -1;
}

void app_error(atomic_int *error, int code)
{
    if (code == ECANCELED && atomic_load(&g_stop)) return;
    int expected = 0;
    atomic_compare_exchange_strong(error, &expected, code ? code : EIO);
    atomic_store(&g_stop, true);
}
