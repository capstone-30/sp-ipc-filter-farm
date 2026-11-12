/* 프로세스별 CSV를 검증하고 한 실행의 통계를 집계한다. 반복 요약은 plot.py 담당. */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double user, sys;
    uint64_t nv, niv, rss0, rss1, first, last;
} cpu_t;
static const char header[] = "backend,scenario,size,clients,filter,depth,count,p50_us,p95_us,p99_us,max_us,mean_us,mbps,imgs,ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,repeat,warmup,mode,payload_bytes,messages_per_s";

static uint64_t number(const char *s)
{
    char *end; errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (!*s || *s == '-' || *end || errno) { fprintf(stderr, "invalid integer: %s\n", s); exit(1); }
    return n;
}

static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double pct(uint64_t *v, size_t n, size_t percent)
{
    size_t rank = (n / 100) * percent + ((n % 100) * percent + 99) / 100;
    return v[rank - 1] / 1000.0;
}

static int load_cpu(const char *path, cpu_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], extra;
    static const char expected[] = "ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,first_send_ns,last_recv_ns\n";
    int valid = fgets(line, sizeof line, f) && !strcmp(line, expected) && fgets(line, sizeof line, f) &&
        sscanf(line, "%lf,%lf,%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 " %c",
            &out->user, &out->sys, &out->nv, &out->niv, &out->rss0, &out->rss1,
            &out->first, &out->last, &extra) == 8 && isfinite(out->user) && isfinite(out->sys) &&
        out->user >= 0 && out->sys >= 0;
    if (fgets(line, sizeof line, f) || ferror(f)) valid = 0;
    if (fclose(f) < 0) return -1;
    if (!valid) { errno = EPROTO; return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--header")) { puts(header); return 0; }
    if (argc != 14) {
        fprintf(stderr, "usage: aggregate dir backend scenario size clients filter depth count warmup repeat mode payload output.csv\n");
        return 1;
    }
    const char *dir = argv[1];
    uint64_t size = number(argv[4]), clients = number(argv[5]), depth = number(argv[7]);
    uint64_t count = number(argv[8]), warmup = number(argv[9]), repeat = number(argv[10]), bytes = number(argv[12]);
    if (!count || !clients || clients > 8 || count > UINT32_MAX || warmup > UINT32_MAX - count ||
        count > SIZE_MAX / clients / sizeof(uint64_t) || bytes > 12u * 1024 * 1024) {
        fprintf(stderr, "aggregate: invalid dimensions\n"); return 1;
    }
    size_t n = (size_t)(count * clients), at = 0;
    uint64_t *v = malloc(n * sizeof *v);
    uint8_t *seen = calloc((size_t)count, 1);
    if (!v || !seen) { free(v); free(seen); perror("aggregate"); return 1; }
    cpu_t sum = {0}; uint64_t first = UINT64_MAX, last = 0;
    char path[4096]; int rc = 1;
    for (uint64_t id = 0; id <= clients; id++) {
        int len = id ? snprintf(path, sizeof path, "%s/client-%" PRIu64 ".csv.cpu.csv", dir, id) :
                       snprintf(path, sizeof path, "%s/server.cpu.csv", dir);
        if (len < 0 || (size_t)len >= sizeof path) { errno = ENAMETOOLONG; goto fail; }
        cpu_t cpu;
        if (load_cpu(path, &cpu) < 0) goto fail;
        sum.user += cpu.user; sum.sys += cpu.sys; sum.nv += cpu.nv; sum.niv += cpu.niv;
        sum.rss0 += cpu.rss0; sum.rss1 += cpu.rss1;
        if (!id) continue;
        if (!cpu.first || cpu.last <= cpu.first) { errno = EPROTO; goto fail; }
        if (cpu.first < first) first = cpu.first;
        if (cpu.last > last) last = cpu.last;
        len = snprintf(path, sizeof path, "%s/client-%" PRIu64 ".csv", dir, id);
        if (len < 0 || (size_t)len >= sizeof path) { errno = ENAMETOOLONG; goto fail; }
        FILE *f = fopen(path, "r");
        if (!f) goto fail;
        char line[256], extra;
        int valid = fgets(line, sizeof line, f) && !strcmp(line, "client_id,seq,rtt_ns,payload_bytes\n");
        memset(seen, 0, (size_t)count);
        uint64_t rows = 0;
        while (valid && fgets(line, sizeof line, f)) {
            uint64_t client, seq, ns, payload;
            if (sscanf(line, "%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 " %c",
                &client, &seq, &ns, &payload, &extra) != 4 || client != id || seq < warmup ||
                seq - warmup >= count || payload != bytes || seen[seq - warmup]) { valid = 0; break; }
            seen[seq - warmup] = 1; rows++; v[at++] = ns;
        }
        if (ferror(f)) valid = 0;
        if (fclose(f) < 0) goto fail;
        if (!valid || rows != count) { errno = EPROTO; goto fail; }
    }
    if (at != n || last <= first) { errno = EPROTO; goto fail; }
    qsort(v, n, sizeof *v, cmp);
    double mean = 0, sec = (last - first) / 1e9;
    for (size_t i = 0; i < n; i++) mean += v[i] / 1000.0 / n;
    FILE *out = fopen(argv[13], "a");
    if (!out) goto fail;
    int saved = fprintf(out, "%s,%s,%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64
        ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 ",%.6f\n", argv[2], argv[3], size, clients, argv[6], depth, count,
        pct(v, n, 50), pct(v, n, 95), pct(v, n, 99), pct(v, n, 100), mean,
        n * (double)bytes / 1e6 / sec, n / sec, sum.user, sum.sys, sum.nv, sum.niv, sum.rss0, sum.rss1,
        repeat, warmup, argv[11], bytes, !strcmp(argv[3], "basic") ? n / sec : 0.0) < 0 ? EIO : 0;
    if (fclose(out) < 0 && !saved) saved = errno;
    if (saved) { errno = saved; goto fail; }
    rc = 0; goto done;
fail:
    perror(path);
done:
    free(v); free(seen); return rc;
}
