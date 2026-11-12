#ifndef PSHM_OWNED_H
#define PSHM_OWNED_H
#include <stdint.h>
#define PSHM_NAME_CAP 96

typedef struct {
    uint32_t magic, reserved;
    uint64_t device, inode;
    char name[PSHM_NAME_CAP];
} pshm_record_t;
int pshm_name(uint32_t team, char name[PSHM_NAME_CAP]);
int pshm_read(int fd, pshm_record_t *r); /* 0 empty, 1 record, -1 error */
int pshm_write(int fd, const pshm_record_t *r);
int pshm_check(int fd, const pshm_record_t *r);
int pshm_remove(const pshm_record_t *r);
#endif
