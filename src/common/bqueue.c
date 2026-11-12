#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "bqueue.h"

/* pthread 함수는 errno가 아니라 반환된 오류 번호를 본다 (SPEC §5.4). */

int bq_init(bqueue_t *q, size_t cap)
{
    if (q == NULL || cap == 0) { errno = EINVAL; return -1; }
    memset(q, 0, sizeof *q);

    q->buf = calloc(cap, sizeof *q->buf);
    if (q->buf == NULL) return -1;
    q->cap = cap;

    int e = pthread_mutex_init(&q->lock, NULL);
    if (e) goto fail_buf;
    e = pthread_cond_init(&q->not_empty, NULL);
    if (e) goto fail_lock;
    e = pthread_cond_init(&q->not_full, NULL);
    if (e) goto fail_empty;
    return 0;

fail_empty: pthread_cond_destroy(&q->not_empty);
fail_lock:  pthread_mutex_destroy(&q->lock);
fail_buf:   free(q->buf); q->buf = NULL;
    errno = e;
    return -1;
}

int bq_push(bqueue_t *q, const job_t *j)
{
    if (pthread_mutex_lock(&q->lock) != 0) return -1;

    while (q->count == q->cap && !q->closed)          /* spurious wakeup 대비 */
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->closed) {                                   /* 종료 중 새 작업 거절 */
        pthread_mutex_unlock(&q->lock);
        errno = ECANCELED;
        return -1;                                     /* 버퍼는 호출자가 해제 */
    }

    q->buf[q->tail] = *j;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int bq_pop(bqueue_t *q, job_t *out)
{
    if (pthread_mutex_lock(&q->lock) != 0) return -1;

    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0) {                               /* closed && empty */
        pthread_mutex_unlock(&q->lock);
        errno = ECANCELED;
        return -1;
    }

    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

void bq_close(bqueue_t *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);             /* 양쪽 모두 깨운다 */
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

void bq_free(bqueue_t *q)
{
    if (q->buf == NULL) return;

    while (q->count > 0) {                             /* join 이후 호출 전제 */
        free(q->buf[q->head].payload);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    free(q->buf);
    q->buf = NULL;

    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}
