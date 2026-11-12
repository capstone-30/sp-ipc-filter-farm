#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/common/timer.h"

static void check_report(samples_t *s, const char *expected)
{
    FILE *capture = tmpfile();
    assert(capture != NULL);
    assert(fflush(stdout) == 0);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    stats_report(s, 2000000, 2.0);
    assert(fflush(stdout) == 0);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    char line[256];
    assert(fgets(line, sizeof line, capture) != NULL);
    assert(strcmp(line, expected) == 0);
    assert(fclose(capture) == 0);
}

int main(void)
{
    samples_t s = {0};
    check_report(&s, "stats: 표본 없음\n");
    stats_add(&s, 7000);
    check_report(&s, "n=1  p50=7.0us p95=7.0us p99=7.0us max=7.0us mean=7.0us  1.0MB/s  0.5img/s\n");
    s.n = 0;
    for (uint64_t i = 100; i > 0; i--) stats_add(&s, i * 1000);
    check_report(&s, "n=100  p50=50.0us p95=95.0us p99=99.0us max=100.0us mean=50.5us  1.0MB/s  50.0img/s\n");
    s.n = 0;
    for (uint64_t i = 3; i > 0; i--) stats_add(&s, i * 1000);
    check_report(&s, "n=3  p50=2.0us p95=3.0us p99=3.0us max=3.0us mean=2.0us  1.0MB/s  1.5img/s\n");
    s.n = 0;
    for (uint64_t i = 0; i < 2049; i++) stats_add(&s, i);
    assert(s.n == 2049 && s.cap >= s.n);
    for (size_t i = 0; i < s.n; i++) assert(s.v[i] == i);
    free(s.v);
    puts("test_timer: OK (nearest-rank·평균·처리량·표본 확장)");
    return 0;
}
