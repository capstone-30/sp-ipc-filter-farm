/* 채널 수명과 상대 종료 — 전 백엔드 공용. 규범: SPEC §5.5. */
#ifndef IPC_META_H
#define IPC_META_H
#include <stdint.h>
#include <sys/types.h>
#include "ipc.h"

/* <key>.meta의 앞 바이트는 잠금 전용이다: byte 0 = 서버, byte client_id = 클라. */
#define META_LOCK_SERVER 0u
#define META_DATA_OFF    16          /* 8클라 잠금과 겹치지 않는다 */
_Static_assert(META_DATA_OFF > SLOT_COUNT, "meta data must follow every client lock byte");

typedef struct {
    uint32_t magic;
    uint32_t ready;
    uint32_t size;
    uint32_t depth;
    int32_t  server_pid;
    uint32_t team;
    uint32_t payload_bytes, slot_count, warmup;
    char     backend[16];
    uint8_t  used[SLOT_COUNT];       /* 0=미사용, 1=사용, 2=퇴역; 실행당 1회만 */
    uint8_t  pad[4];
} meta_t;

int    meta_open   (const char *key_path);              /* fd 또는 -1 */
int    meta_remove (const char *key_path);              /* destroy 전용 */
int    meta_trylock(int fd, uint32_t byte);             /* 이미 잡혀 있으면 -1 */
pid_t  meta_owner  (int fd, uint32_t byte);             /* 0 = 잠금 없음 */
int    meta_store  (int fd, const meta_t *m);
int    meta_load   (int fd, meta_t *m);
int    meta_wait_ready(int fd, meta_t *m, unsigned timeout_ms);
int    meta_claim_id(int fd, uint32_t client_id);       /* used 검사·설정 */
int    meta_retire_id(int fd, uint32_t client_id);      /* 서버: used=2 게시 */
int    meta_id_retired(int fd, uint32_t client_id);     /* 1=퇴역, 0=아님, -1=오류 */
/* 서버(id=0)는 게시, 클라는 준비 대기·검증·ID 확보. 백엔드 공용 진입 절차. */
int    meta_begin(int fd, uint32_t id, const char *backend,
                  const ipc_config_t *cfg, meta_t *m);
#endif
