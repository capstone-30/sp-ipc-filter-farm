#include <string.h>
#include "ipc.h"

/* 응용은 이 표를 통해 백엔드를 선택한다 (R7). */
#ifndef IPC_BACKENDS
#define IPC_BACKENDS 31
#endif
extern const ipc_backend_t ipc_backend_pipe;
extern const ipc_backend_t ipc_backend_fifo;
extern const ipc_backend_t ipc_backend_sysv_mq;
extern const ipc_backend_t ipc_backend_sysv_shm;
extern const ipc_backend_t ipc_backend_posix_shm;

static const ipc_backend_t *const backends[] = {
#if IPC_BACKENDS & 1
    &ipc_backend_pipe,
#endif
#if IPC_BACKENDS & 2
    &ipc_backend_fifo,
#endif
#if IPC_BACKENDS & 4
    &ipc_backend_sysv_mq,
#endif
#if IPC_BACKENDS & 8
    &ipc_backend_sysv_shm,
#endif
#if IPC_BACKENDS & 16
    &ipc_backend_posix_shm,
#endif
};

const ipc_backend_t *ipc_lookup(const char *name)
{
    if (name == NULL) return NULL;

    for (size_t i = 0; i < sizeof backends / sizeof backends[0]; i++)
        if (strcmp(backends[i]->name, name) == 0)
            return backends[i];

    return NULL;
}
