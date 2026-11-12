#ifndef APP_H
#define APP_H
#include <semaphore.h>
#include <signal.h>
#include "../ipc/ipc.h"

typedef struct {
    const ipc_backend_t *backend;
    const char *key, *csv, *cpu_csv;
    ipc_config_t ipc;
    uint32_t workers, clients, count, id, filter, warmup;
    int no_save, quiet, inplace_gray;
} options_t;

int app_options(int argc, char **argv, int server, options_t *out);
int app_signals(sigset_t *set);
int app_tick(const sigset_t *set); /* SIGINT=1, timeout=0, error=-1 */
int app_acquire(sem_t *sem);
void app_error(atomic_int *error, int code);
int app_valid_payload(const msg_hdr_t *h, uint32_t type, const options_t *o);
#endif
