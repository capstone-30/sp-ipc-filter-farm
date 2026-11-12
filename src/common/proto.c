#include "proto.h"

uint32_t fnv1a32(const void *buf, size_t n)
{
    const uint8_t *p = buf;
    uint32_t h = 2166136261u;          /* FNV offset basis */

    for (size_t i = 0; i < n; i++) {
        h ^= p[i];                     /* FNV-1a: xor 먼저, 그 다음 곱셈 */
        h *= 16777619u;                /* FNV prime */
    }
    return h;
}
