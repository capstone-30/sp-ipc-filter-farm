#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/common/filter.h"
#include "../src/common/ppm.h"

int main(void)
{
    uint32_t w, h, ow, oh;
    uint8_t *in = ppm_load("client-1-input.ppm", &w, &h);
    uint8_t *out = ppm_load("client-1-output.ppm", &ow, &oh);
    assert(in && out && w == 256 && h == 256 && ow == w && oh == h);
    size_t bytes = (size_t)w * h * 3;
    uint8_t *expected = malloc(bytes), *generated = ppm_gen(w, h);
    assert(expected && generated);
    assert(memcmp(in, generated, bytes) == 0);
    filter_gray(in, expected, w, h);
    assert(memcmp(out, expected, bytes) == 0);
    assert(memcmp(in, out, bytes) != 0);
    free(in); free(out); free(expected); free(generated);
    puts("test_images: OK (256x256 입력·GRAY 출력 전 바이트 비교)");
    return 0;
}
