/* 생산자 4 + 소비자 4로 100만 건. 합계·개수 일치, 종료 시 전 스레드 join. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../src/common/bqueue.h"

#define PROD 4
#define CONS 4
#define PER  250000                       /* 4 × 250,000 = 1,000,000 */

static bqueue_t q;
static uint64_t csum[CONS];
static size_t   ccnt[CONS];

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 1; i <= PER; i++) {
        job_t j = { .hdr = { .seq = i }, .payload = NULL };
        assert(bq_push(&q, &j) == 0);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    size_t k = (size_t)(uintptr_t)arg;
    job_t j;
    while (bq_pop(&q, &j) == 0) { csum[k] += j.hdr.seq; ccnt[k]++; }
    return NULL;
}

int main(void)
{
    pthread_t p[PROD], c[CONS];
    assert(bq_init(&q, 1024) == 0);

    for (size_t i = 0; i < CONS; i++)
        assert(pthread_create(&c[i], NULL, consumer, (void *)(uintptr_t)i) == 0);
    for (size_t i = 0; i < PROD; i++)
        assert(pthread_create(&p[i], NULL, producer, NULL) == 0);

    for (size_t i = 0; i < PROD; i++) pthread_join(p[i], NULL);
    bq_close(&q);                          /* 생산 종료 후에만 닫는다 */
    for (size_t i = 0; i < CONS; i++) pthread_join(c[i], NULL);

    uint64_t sum = 0; size_t cnt = 0;
    for (size_t i = 0; i < CONS; i++) { sum += csum[i]; cnt += ccnt[i]; }

    assert(cnt == (size_t)PROD * PER);
    assert(sum == (uint64_t)PROD * PER * (PER + 1) / 2);

    bq_free(&q);
    printf("test_bqueue: OK (%zu건, 합계 %llu)\n", cnt, (unsigned long long)sum);
    return 0;
}
