/* 계약 · 변경 금지. 설계 전문: SPEC §4.2. 구현은 ipc_sysv_mq.c (Phase 3). */
#ifndef IPC_SYSV_MQ_H
#define IPC_SYSV_MQ_H
#include <stddef.h>
#include <stdint.h>
#include "../common/proto.h"

#define CHUNK_PAYLOAD 8000        /* MSGMAX 기본 8192B 이하로 유지 */

typedef struct {
    long     mtype;               /* 1(요청) 또는 client_id(응답) */
    uint32_t client_id;           /* 모든 청크에 필수: 1..SLOT_COUNT */
    uint32_t seq;
    uint32_t chunk_idx;           /* 0-based */
    uint32_t chunk_cnt;
    uint32_t bytes;               /* 이 청크의 유효 바이트, ≤ CHUNK_PAYLOAD */
    uint8_t  data[CHUNK_PAYLOAD]; /* 0번 청크는 앞 48바이트가 msg_hdr_t */
} mq_chunk_t;

/* msgsz는 아래 식으로 계산한다. sizeof(mq_chunk_t)는 꼬리 padding을 포함할 수
 * 있으므로 쓰지 않는다. */
#define MQ_MSGSZ(bytes) (offsetof(mq_chunk_t, data) - sizeof(long) + (size_t)(bytes))

_Static_assert(MQ_MSGSZ(CHUNK_PAYLOAD) == 8020, "청크 최대 msgsz는 8020B");
_Static_assert(CHUNK_PAYLOAD >= sizeof(msg_hdr_t), "0번 청크에 헤더가 들어가야 한다");
#endif
