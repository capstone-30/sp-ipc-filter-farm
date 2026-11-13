#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ipc/ipc.h"
#include "../src/common/filter.h"

atomic_bool g_stop = false;

int main(int argc, char **argv)
{
    assert(argc >= 2 && argc <= 4);
    const ipc_backend_t *b = ipc_lookup(argc >= 3 ? argv[2] : "fifo");
    ipc_config_t cfg = {.size=64, .depth=1, .team=1};
    ipc_channel_t *ch = b->client_open(argv[1], 1, &cfg);
    assert(ch);
    if (argc == 4) {
        assert(!strcmp(argv[3], "--bad-bye"));
        msg_hdr_t bad = {.magic = IPC_MAGIC, .type = MSG_BYE, .client_id = 1,
            .checksum = fnv1a32(NULL, 0) ^ 1u};
        assert(b->send(ch, &bad, NULL) == 0);
        b->close(ch);
        return 0;
    }
    uint8_t src[64 * 64 * 3], rx[sizeof src], expected[sizeof src];
    for (size_t i = 0; i < sizeof src; i++) src[i] = (uint8_t)i;
    msg_hdr_t request = { .magic = IPC_MAGIC, .type = MSG_REQ, .client_id = 1,
        .width = 64, .height = 64, .channels = 3, .filter = F_GRAY,
        .payload_len = sizeof src, .checksum = fnv1a32(src, sizeof src) };
    for (unsigned i = 0; i < 8; i++) {
        msg_hdr_t bad = request, response;
        bad.seq = i;
        switch (i) {
        case 0: bad.width--; break;
        case 1: bad.height--; break;
        case 2: bad.channels = 4; break;
        case 3: bad.filter = 99; break;
        case 4: bad.type = MSG_RESP; break;
        case 5: bad.payload_len--; break;
        case 6: bad.checksum ^= 1; break;
        case 7: bad.type = MSG_BYE; break; /* BYE는 0바이트만 허용 */
        }
        assert(b->send(ch, &bad, src) == 0);
        assert(b->recv(ch, &response, rx, sizeof rx) == 0);
        assert(response.magic == IPC_MAGIC && response.client_id == 1 && response.seq == i);
        assert(response.type == MSG_ERR && response.payload_len == 0);
    }
    request.seq = 8;
    assert(b->send(ch, &request, src) == 0);
    msg_hdr_t response;
    assert(b->recv(ch, &response, rx, sizeof rx) == 0);
    assert(response.type == MSG_RESP && response.seq == 8 && response.client_id == 1);
    assert(response.payload_len == sizeof rx && fnv1a32(rx, sizeof rx) == response.checksum);
    filter_gray(src, expected, 64, 64);
    assert(memcmp(rx, expected, sizeof rx) == 0);
    request.type = MSG_BYE; request.payload_len = 0;
    request.checksum = fnv1a32(NULL, 0);
    assert(b->send(ch, &request, NULL) == 0);
    b->close(ch);
    puts("test_app_protocol: OK (잘못된 요청 8종 MSG_ERR·후속 정상 응답·BYE)");
    return 0;
}
