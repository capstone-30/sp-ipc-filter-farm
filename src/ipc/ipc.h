/* 백엔드 공용 계약. 변경은 spec.md에 먼저 반영한다. */
#ifndef IPC_H
#define IPC_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "../common/proto.h"

typedef struct ipc_channel ipc_channel_t;   /* 백엔드별 불투명 타입 */

typedef struct {
    uint32_t size;             /* 정사각 이미지 한 변, 기본 512, 1..2048 */
    uint32_t depth;            /* 클라이언트별 in-flight 상한, 기본 1, 1..16 */
    uint32_t team;             /* 팀 번호, 기본 1, 1..99 */
    uint32_t payload_bytes;    /* 0=이미지, 기초 벤치는 지정 바이트 */
    uint32_t warmup;           /* 측정 경계 일치 검사 */
} ipc_config_t;

static inline bool ipc_config_valid(const ipc_config_t *cfg)
{
    return cfg && cfg->size >= 1 && cfg->size <= 2048 &&
        cfg->depth >= 1 && cfg->depth <= 16 && cfg->team >= 1 && cfg->team <= 99 &&
        cfg->payload_bytes <= 1048576;
}

static inline size_t ipc_payload_size(const ipc_config_t *cfg)
{
    return cfg->payload_bytes ? cfg->payload_bytes : (size_t)cfg->size * cfg->size * 3;
}

typedef struct {
    const char *name;                       /* pipe / FIFO / MQ / SHM 백엔드 이름 */

    ipc_channel_t *(*server_open)(const char *key_path, const ipc_config_t *cfg);
    ipc_channel_t *(*client_open)(const char *key_path, uint32_t client_id,
                                  const ipc_config_t *cfg);

    /* 성공 0, 실패 -1(errno). 블로킹. 스레드 안전 필수:
     * 백엔드는 송신 뮤텍스와 수신 뮤텍스를 분리해 둔다. 하나로 묶으면
     * 송수신이 직렬화되어 측정값이 왜곡된다. */
    int  (*send)(ipc_channel_t *ch, const msg_hdr_t *h, const void *payload);
    int  (*recv)(ipc_channel_t *ch, msg_hdr_t *h, void *payload, size_t cap);

    void (*close)(ipc_channel_t *ch);       /* detach + fd/채널 메모리 해제 */
    void (*destroy)(ipc_channel_t *ch);     /* 서버 전용: IPC_RMID / shm_unlink */
} ipc_backend_t;

const ipc_backend_t *ipc_lookup(const char *name);   /* 미지원 이름이면 NULL */

/* 종료 플래그. 백엔드는 참조만 한다. ipc/ 아래 .c를 링크하는 모든 바이너리가 각각
 *     atomic_bool g_stop = false;
 * 를 1개씩 정의한다 — server.c, client.c, tests/test_ipc_loopback.c. 빠지면 링크 에러.
 * ATOMIC_VAR_INIT은 C17 deprecated·C23 삭제라 -Werror와 함께 쓰면 빌드가 깨진다. */
extern atomic_bool g_stop;
#endif
