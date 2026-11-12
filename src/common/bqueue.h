/* 계약 · 변경 금지. 근거: SPEC §5.2. 구현은 bqueue.c (Phase 1). */
#ifndef BQUEUE_H
#define BQUEUE_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "proto.h"

/* DECISION: 문서가 job_t를 정의하지 않아 가장 단순한 형태로 둔다 —
 * 헤더 + 소유 페이로드 포인터. 큐가 소유권을 옮기고, push 실패 시 호출자가
 * 해제하며, 종료 시 큐 잔여 항목은 bq_free가 해제한다 (SPEC §5.3). */
typedef struct {
    msg_hdr_t  hdr;
    uint8_t   *payload;   /* malloc 소유. payload_len 바이트 */
} job_t;

typedef struct {
    job_t          *buf;
    size_t          cap, head, tail, count;
    int             closed;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bqueue_t;

int  bq_init (bqueue_t *q, size_t cap);
int  bq_push (bqueue_t *q, const job_t *j);  /* full이면 대기, closed면 -1 */
int  bq_pop  (bqueue_t *q, job_t *out);      /* empty면 대기, closed&&empty면 -1 */
void bq_close(bqueue_t *q);                  /* closed=1 후 broadcast */
void bq_free (bqueue_t *q);                  /* 잔여 항목의 payload까지 해제 */
#endif
