#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "resource.h"

static int rss(long *value)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld kB", value) == 1) { found = 1; break; }
    int e = ferror(f) ? EIO : 0;
    if (fclose(f) < 0 && !e) e = errno;
    if (!found && !e) e = EPROTO;
    if (e) { errno = e; return -1; }
    return 0;
}

int resource_start(resource_t *r)
{
    /* 시작 RSS 읽기는 CPU 구간 밖, 종료 RSS 읽기도 CPU 구간 밖이다. */
    if (rss(&r->rss_start) < 0 || getrusage(RUSAGE_SELF, &r->start) < 0) return -1;
    r->started = 1;
    return 0;
}

int resource_end(resource_t *r)
{
    if (!r->started) { errno = EINVAL; return -1; }
    if (getrusage(RUSAGE_SELF, &r->end) < 0 || rss(&r->rss_end) < 0) return -1;
    r->ended = 1;
    return 0;
}

#ifdef __linux__
static double ms(struct timeval t) { return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; }
#endif

int resource_save(const resource_t *r, const char *path, uint64_t first, uint64_t last)
{
#ifndef __linux__
    (void)r; (void)path; (void)first; (void)last;
    errno = ENOTSUP; return -1;
#else
    if (!r->ended) { errno = EINVAL; return -1; }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = fprintf(f, "ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,first_send_ns,last_recv_ns\n"
        "%.6f,%.6f,%ld,%ld,%ld,%ld,%" PRIu64 ",%" PRIu64 "\n",
        ms(r->end.ru_utime) - ms(r->start.ru_utime), ms(r->end.ru_stime) - ms(r->start.ru_stime),
        r->end.ru_nvcsw - r->start.ru_nvcsw, r->end.ru_nivcsw - r->start.ru_nivcsw,
        r->rss_start, r->rss_end, first, last);
    int e = rc < 0 ? (errno ? errno : EIO) : 0;
    if (fclose(f) < 0 && !e) e = errno;
    if (e) { errno = e; return -1; }
    return 0;
#endif
}
