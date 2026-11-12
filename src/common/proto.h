/* 계약 · 변경 금지. 근거: SPEC §3. */
#ifndef PROTO_H
#define PROTO_H
#include <stddef.h>   /* size_t */
#include <stdint.h>

#define IPC_MAGIC   0x49504331u   /* "IPC1" */
#define MAX_PAYLOAD (12u << 20)   /* 12MB = 2048*2048*3 */
#ifndef SLOT_COUNT
#define SLOT_COUNT  4             /* 응용 4, 기초 벤치 빌드는 8 */
#endif

typedef enum { MSG_REQ = 1, MSG_RESP = 2, MSG_ERR = 3, MSG_BYE = 4 } msg_type_t;
typedef enum { F_GRAY = 0, F_BLUR = 1, F_EDGE = 2 } filter_t;

typedef struct {
    uint32_t magic;        /* IPC_MAGIC */
    uint32_t type;         /* msg_type_t */
    uint32_t client_id;    /* 1..SLOT_COUNT, 응답 라우팅 키. 링 인덱스는 client_id - 1 */
    uint32_t seq;          /* 요청 일련번호, RTT 매칭 */
    uint32_t filter;       /* filter_t */
    uint32_t width;
    uint32_t height;
    uint32_t channels;     /* 항상 3 */
    uint32_t payload_len;  /* width * height * channels */
    uint32_t checksum;     /* payload FNV-1a 32bit */
    uint64_t t_send_ns;    /* CLOCK_MONOTONIC */
} msg_hdr_t;

_Static_assert(sizeof(msg_hdr_t) == 48, "msg_hdr_t must be 48 bytes");

uint32_t fnv1a32(const void *buf, size_t n);
#endif
