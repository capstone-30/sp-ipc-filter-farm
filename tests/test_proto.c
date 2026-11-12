/* Phase 0 게이트: 헤더 크기(_Static_assert)와 체크섬 결정성. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/common/proto.h"

int main(void)
{
    /* FNV-1a 32bit 공개 테스트 벡터. FNV-1(곱셈 먼저)로 잘못 짜면 여기서 걸린다. */
    assert(fnv1a32("", 0)             == 0x811c9dc5u);
    assert(fnv1a32("a", 1)            == 0xe40c292cu);
    assert(fnv1a32("foobar", 6)       == 0xbf9cf968u);

    /* 결정성: 같은 입력은 항상 같은 값 */
    static uint8_t buf[4096];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31u + 7u);
    assert(fnv1a32(buf, sizeof buf) == fnv1a32(buf, sizeof buf));

    /* 한 바이트만 바뀌어도, 순서만 바뀌어도 값이 달라진다 */
    uint32_t base = fnv1a32(buf, sizeof buf);
    buf[1234] ^= 0x01u;
    assert(fnv1a32(buf, sizeof buf) != base);
    assert(fnv1a32("ab", 2) != fnv1a32("ba", 2));

    /* 헤더 계약 */
    assert(sizeof(msg_hdr_t) == 48);
    assert(MAX_PAYLOAD == 2048u * 2048u * 3u);

    printf("test_proto: OK (msg_hdr_t=%zuB)\n", sizeof(msg_hdr_t));
    return 0;
}
