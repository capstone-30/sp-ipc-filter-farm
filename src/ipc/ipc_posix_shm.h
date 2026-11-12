/* 계약 · 변경 금지. 설계 전문: SPEC §4.4. 구현은 ipc_posix_shm.c (Phase 5). */
#ifndef IPC_POSIX_SHM_H
#define IPC_POSIX_SHM_H
#include <semaphore.h>
#include <stdint.h>
#include "../common/proto.h"

/* SysV(§4.3)의 링·복사 횟수·인덱싱·초기 카운터를 그대로 유지하고 생성 API와
 * 세마포어 구현만 다르다. named semaphore는 쓰지 않는다:
 * empty는 W, full은 0으로 sem_init(&s, 1, value). */
typedef struct {
    uint32_t magic;
    uint32_t slot_count;
    uint32_t depth;
    uint32_t reserved;          /* 0 */
    uint64_t slot_bytes;
    struct {
        sem_t req_empty, req_full;
        sem_t resp_empty, resp_full;
    } sems[SLOT_COUNT];
    /* 이후 req/resp 링. stride = align_up(48 + slot_bytes, _Alignof(msg_hdr_t)). */
} pshm_hdr_t;

_Static_assert(sizeof(pshm_hdr_t) % _Alignof(msg_hdr_t) == 0,
               "헤더 뒤 링 데이터가 msg_hdr_t 정렬을 지켜야 한다");
#endif
