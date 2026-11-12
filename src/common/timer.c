#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "timer.h"

/* RTT 샘플은 배열에 그대로 쌓고 종료 시 qsort로 백분위를 뽑는다.
 * 1,000 샘플 = 8KB. 온라인 근사(t-digest 등)를 쓸 이유가 없다 (SPEC §6). */

void stats_add(samples_t *s, uint64_t ns)
{
    if (s->n == s->cap) {
        if (s->cap > SIZE_MAX / sizeof *s->v / 2) {
            errno = EOVERFLOW; perror("stats_add"); exit(EXIT_FAILURE);
        }
        size_t cap = s->cap ? s->cap * 2 : 1024;
        uint64_t *v = realloc(s->v, cap * sizeof *v);
        if (v == NULL) { perror("stats_add"); exit(EXIT_FAILURE); }
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = ns;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct(const samples_t *s, size_t percent) /* nearest-rank */
{
    size_t rank = (s->n / 100) * percent + ((s->n % 100) * percent + 99) / 100;
    return s->v[rank - 1];
}

void stats_report(samples_t *s, uint64_t bytes_total, double wall_sec)
{
    if (s->n == 0) { printf("stats: 표본 없음\n"); return; }

    qsort(s->v, s->n, sizeof *s->v, cmp_u64);

    double mean = 0;
    for (size_t i = 0; i < s->n; i++) mean += (double)s->v[i];
    mean /= (double)s->n;

    /* 평균만 보고하지 않는다 (R6). MB = 1,000,000B. */
    printf("n=%zu  p50=%.1fus p95=%.1fus p99=%.1fus max=%.1fus mean=%.1fus"
           "  %.1fMB/s  %.1fimg/s\n",
           s->n,
           pct(s, 50) / 1e3, pct(s, 95) / 1e3, pct(s, 99) / 1e3,
           pct(s, 100) / 1e3, mean / 1e3,
           wall_sec > 0 ? (double)bytes_total / 1e6 / wall_sec : 0.0,
           wall_sec > 0 ? (double)s->n / wall_sec : 0.0);
}
