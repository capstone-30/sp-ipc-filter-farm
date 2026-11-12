#ifndef RESOURCE_H
#define RESOURCE_H
#include <stdint.h>
#include <sys/resource.h>

typedef struct {
    struct rusage start, end;
    long rss_start, rss_end;
    int started, ended;
} resource_t;
int resource_start(resource_t *r);
int resource_end(resource_t *r);
int resource_save(const resource_t *r, const char *path, uint64_t first, uint64_t last);
#endif
