/* 계약 · 변경 금지. 설계 전문: SPEC §4.3. 구현은 ipc_sysv_shm.c (Phase 4). */
#ifndef IPC_SYSV_SHM_H
#define IPC_SYSV_SHM_H
#include <stdint.h>
#include "../common/proto.h"

/* 공유 영역 = shm_hdr_t + 요청 링 + 응답 링.
 *   req [client_id - 1][0..W-1] : msg_hdr_t + payload[slot_bytes]
 *   resp[client_id - 1][0..W-1] : msg_hdr_t + payload[slot_bytes]
 * 요청·응답은 서로 다른 링이고, 슬롯 인덱스는 그 링의 단조 증가 head/tail % W다
 * (seq % W를 쓰지 않는다 — 워커 완료 순서가 바뀐다).
 * stride = align_up(48 + slot_bytes, _Alignof(msg_hdr_t)).
 * 크기 = 2 * SLOT_COUNT * W * stride + sizeof(shm_hdr_t). */
typedef struct {
    uint32_t magic;               /* IPC_MAGIC */
    uint32_t slot_count;          /* SLOT_COUNT */
    uint32_t depth;               /* W = cfg->depth */
    uint32_t reserved;            /* 0 */
    uint64_t slot_bytes;          /* ipc_payload_size(cfg): 이미지 또는 기초 데이터 */
} shm_hdr_t;

/* 세마포어 배열은 4 * SLOT_COUNT개. W에 따라 개수를 늘리지 않는다.
 * i = client_id - 1 에 대해:
 *   4i+SEM_REQ_EMPTY  초기값 W  |  4i+SEM_REQ_FULL   초기값 0
 *   4i+SEM_RESP_EMPTY 초기값 W  |  4i+SEM_RESP_FULL  초기값 0
 * SEM_UNDO는 쓰지 않는다. sem_flg = IPC_NOWAIT + SPEC §5.4의 유한 대기. */
enum { SEM_REQ_EMPTY = 0, SEM_REQ_FULL, SEM_RESP_EMPTY, SEM_RESP_FULL, SEM_PER_SLOT };

#define SHM_SEM_COUNT   (SEM_PER_SLOT * SLOT_COUNT)
#define SHM_SEM(i, which) ((int)((i) * SEM_PER_SLOT + (which)))

_Static_assert(sizeof(shm_hdr_t) % _Alignof(msg_hdr_t) == 0,
               "헤더 뒤 링 데이터가 msg_hdr_t 정렬을 지켜야 한다");
#endif
