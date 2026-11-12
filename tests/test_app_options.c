#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/common/app.h"
#ifndef BASIC_BENCH
#define BASIC_BENCH 0
#endif

atomic_bool g_stop = false;

static void options(void)
{
    options_t o;
    char *args[] = {"client", "--ipc=fifo", "--id=1", "--filter=gray",
        "--count=4294967294", "--warmup=1", "--quiet"};
    assert(app_options(7, args, 0, &o) == 0);
    assert(o.count + o.warmup == UINT32_MAX && o.quiet && o.ipc.warmup == 1);
    args[4] = "--count=4294967295";
    assert(app_options(7, args, 0, &o) == -1 && errno == EINVAL);
    args[5] = "--warmup=0";
    assert(app_options(7, args, 0, &o) == 0 && o.count == UINT32_MAX && !o.warmup);
    args[6] = "--warmup=0";
    assert(app_options(7, args, 0, &o) == -1 && errno == EINVAL);
    args[4] = "--count=0";
    assert(app_options(5, args, 0, &o) == -1 && errno == EINVAL);
}

static void payloads(void)
{
    options_t o = {.ipc = {.size = 2048}};
    msg_hdr_t good = {.magic = IPC_MAGIC, .type = MSG_REQ, .client_id = SLOT_COUNT,
        .width = 2048, .height = 2048, .channels = 3, .payload_len = MAX_PAYLOAD};
    for (unsigned basic = 0; basic < 2; basic++) {
        o.ipc.payload_bytes = basic ? 64 : 0;
        good.payload_len = basic ? 64 : MAX_PAYLOAD;
        for (uint32_t filter = F_GRAY; filter <= F_EDGE + 1; filter++) {
            good.filter = filter;
            assert(app_valid_payload(&good, MSG_REQ, &o) == (filter <= (basic ? F_GRAY : F_EDGE)));
        }
        good.filter = F_GRAY;
        for (unsigned field = 0; field < 9; field++) {
            msg_hdr_t bad = good;
            switch (field) {
            case 0: bad.magic = 0; break;
            case 1: bad.type = MSG_RESP; break;
            case 2: bad.client_id = 0; break;
            case 3: bad.client_id = SLOT_COUNT + 1; break;
            case 4: bad.width = UINT32_MAX; break;
            case 5: bad.height--; break;
            case 6: bad.channels = 4; break;
            case 7: bad.payload_len--; break;
            case 8: bad.payload_len++; break;
            }
            assert(!app_valid_payload(&bad, MSG_REQ, &o));
        }
    }
    o.ipc.size = good.width = good.height = 1;
    o.ipc.payload_bytes = 0;
    good.payload_len = 3;
    assert(app_valid_payload(&good, MSG_REQ, &o));
}

static void numeric_options(void)
{
    const struct {
        const char *name, *min, *max, *below, *above;
        int server, enabled;
    } cases[] = {
        {"team", "1", "99", "0", "100", 0, 1},
        {"size", "1", "2048", "0", "2049", 0, 1},
        {"depth", "1", "16", "0", "17", 0, 1},
        {"count", "1", "4294967295", "0", "4294967296", 0, 1},
        {"workers", "1", "4294967295", "0", "4294967296", 1, 1},
        {"clients", "1", SLOT_COUNT == 8 ? "8" : "4", "0", SLOT_COUNT == 8 ? "9" : "5", 1, 1},
        {"id", "1", SLOT_COUNT == 8 ? "8" : "4", "0", SLOT_COUNT == 8 ? "9" : "5", 0, 1},
        {"warmup", "0", "4294967294", "00", "4294967295", 0, 1},
        {"bytes", "64", "1048576", "63", "1048577", 0, BASIC_BENCH}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *values[] = {cases[i].min, cases[i].max, cases[i].below,
            cases[i].above, "", "-1", "+1", "1x", " 1", "18446744073709551616"};
        for (size_t j = 0; j < sizeof values / sizeof values[0]; j++) {
            char option[80];
            assert(snprintf(option, sizeof option, "--%s=%s", cases[i].name, values[j]) > 0);
            char *args[] = {"app", "--ipc=fifo", "--id=1", "--filter=gray", "--count=1", option, option};
            int n = cases[i].server ? 2 : 5;
            /* 필수 옵션을 바꿀 때 중복으로 넣지 않는다. */
            if (strcmp(cases[i].name, "id") == 0) args[2] = option;
            else if (strcmp(cases[i].name, "count") == 0) args[4] = option;
            else args[n++] = option;
            options_t o;
            int valid = j < 2 && cases[i].enabled;
            assert(app_options(n, args, cases[i].server, &o) == (valid ? 0 : -1));
            if (!valid) { assert(errno == EINVAL); continue; }
            uint32_t actual[] = {o.ipc.team, o.ipc.size, o.ipc.depth, o.count,
                o.workers, o.clients, o.id, o.warmup, o.ipc.payload_bytes};
            assert(actual[i] == strtoul(values[j], NULL, 10));
            args[n] = option;
            assert(app_options(n + 1, args, cases[i].server, &o) == -1 && errno == EINVAL);
        }
    }
    options_t o;
    char *wrong_client[] = {"client", "--ipc=fifo", "--id=1", "--filter=gray", "--count=1", "--workers=2"};
    assert(app_options(6, wrong_client, 0, &o) == -1 && errno == EINVAL);
    wrong_client[5] = "--clients=1";
    assert(app_options(6, wrong_client, 0, &o) == -1 && errno == EINVAL);
    char *wrong_server[] = {"server", "--ipc=fifo", "--id=1"};
    assert(app_options(3, wrong_server, 1, &o) == -1 && errno == EINVAL);
}

int main(void)
{
    options();
    numeric_options();
    payloads();
    puts("test_app_options: OK (numeric bounds, malformed values, duplicates, roles, image/basic payloads)");
    return 0;
}
