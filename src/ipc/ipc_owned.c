#include <errno.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ipc_owned.h"
#include "ipc_util.h"

int owned_read(int fd, ipc_record_t *r)
{
    struct stat st;
    if (ipc_stat_owned(fd, &st) < 0) return -1;
    if (!st.st_size) return 0;
    if (st.st_size != sizeof *r) { errno = EPROTO; return -1; }
    if (ipc_pio(fd, r, sizeof *r, 0, 0) < 0) return -1;
    if (r->magic != IPC_MAGIC || r->ids[0] < -1 || r->ids[1] < -1 ||
        (r->key_owned != 0 && r->key_owned != 1)) { errno = EPROTO; return -1; }
    return 1;
}
int owned_write(int fd, const ipc_record_t *r)
{
    return ipc_pio(fd, (void *)r, sizeof *r, 0, 1);
}
int owned_remove(int kind, const ipc_record_t *r)
{
    int present[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        if (r->ids[i] < 0) continue;
        int id = kind == OWN_MQ ? msgget(r->keys[i], 0) :
            (i == 0 ? shmget(r->keys[i], 0, 0) : semget(r->keys[i], 0, 0));
        if (id < 0) { if (errno == ENOENT) continue; return -1; }
        if (id != r->ids[i]) { errno = EEXIST; return -1; }
        uid_t owner;
        if (kind == OWN_MQ) {
            struct msqid_ds ds;
            if (msgctl(id, IPC_STAT, &ds) < 0) return -1;
            owner = ds.msg_perm.cuid;
        } else if (i == 0) {
            struct shmid_ds ds;
            if (shmctl(id, IPC_STAT, &ds) < 0) return -1;
            owner = ds.shm_perm.cuid;
        } else {
            struct semid_ds ds;
            union { int val; struct semid_ds *buf; unsigned short *array; } arg = {.buf = &ds};
            if (semctl(id, 0, IPC_STAT, arg) < 0) return -1;
            owner = ds.sem_perm.cuid;
        }
        if (owner != geteuid()) { errno = EPERM; return -1; }
        present[i] = 1;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (!present[i]) continue;
        int rc = kind == OWN_MQ ? msgctl(r->ids[i], IPC_RMID, NULL) :
            (i == 0 ? shmctl(r->ids[i], IPC_RMID, NULL) : semctl(r->ids[i], 0, IPC_RMID));
        if (rc < 0 && errno != EINVAL && errno != EIDRM) return -1;
    }
    return 0;
}
