#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pshm_owned.h"
#include "ipc_util.h"
#include "../common/proto.h"

int pshm_name(uint32_t team, char name[PSHM_NAME_CAP])
{
    if (!team || team > 99) { errno = EINVAL; return -1; }
    int n = snprintf(name, PSHM_NAME_CAP, "/osproject_%u_shm", team);
    if (n < 0 || n >= PSHM_NAME_CAP) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
int pshm_read(int fd, pshm_record_t *r)
{
    struct stat st;
    if (ipc_stat_owned(fd, &st) < 0) return -1;
    if (!st.st_size) return 0;
    if (st.st_size != sizeof *r) { errno = EPROTO; return -1; }
    if (ipc_pio(fd, r, sizeof *r, 0, 0) < 0) return -1;
    if (r->magic != IPC_MAGIC || r->reserved || !r->inode ||
        !memchr(r->name, 0, sizeof r->name)) {
        errno = EPROTO; return -1;
    }
    char expected[PSHM_NAME_CAP];
    unsigned team;
    if (sscanf(r->name, "/osproject_%2u", &team) != 1 ||
        pshm_name(team, expected) < 0 || strcmp(r->name, expected)) {
        errno = EPROTO; return -1;
    }
    return 1;
}
int pshm_write(int fd, const pshm_record_t *r)
{
    return ipc_pio(fd, (void *)r, sizeof *r, 0, 1);
}
int pshm_check(int fd, const pshm_record_t *r)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (uint64_t)st.st_dev != r->device || (uint64_t)st.st_ino != r->inode) { errno = EEXIST; return -1; }
    return 0;
}
int pshm_remove(const pshm_record_t *r)
{
    int fd = shm_open(r->name, O_RDONLY, 0);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    int e = 0;
    if (pshm_check(fd, r) < 0) e = errno;
    else if (shm_unlink(r->name) < 0 && errno != ENOENT) e = errno;
    if (close(fd) < 0 && !e) e = errno;
    if (e) { errno = e; return -1; }
    return 0;
}
