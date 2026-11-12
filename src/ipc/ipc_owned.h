#ifndef IPC_OWNED_H
#define IPC_OWNED_H
#include <sys/ipc.h>
#include "ipc.h"
/* Phase 3 .mq 바이너리 형식 유지. SHM은 ids[0]=shmid, ids[1]=semid. */
typedef struct { uint32_t magic; int ids[2], key_owned; key_t keys[2]; } ipc_record_t;
enum { OWN_MQ, OWN_SHM };
int owned_read(int fd, ipc_record_t *r); /* 1=읽음, 0=빈 파일, -1=오류 */
int owned_write(int fd, const ipc_record_t *r);
int owned_remove(int kind, const ipc_record_t *r);
#endif
