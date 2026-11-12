#ifndef IPC_NAMES_H
#define IPC_NAMES_H
#include <errno.h>
#include <stdint.h>
#include <sys/ipc.h>

/* 과제의 60xxy: xx=팀 번호, y=MQ 요청/응답(0/1), SHM/SEM(2/3). */
static inline key_t ipc_team_key(uint32_t team, unsigned purpose)
{
    if (!team || team > 99 || purpose > 3) { errno = EINVAL; return (key_t)-1; }
    return (key_t)(60000 + 10 * team + purpose);
}
#endif
