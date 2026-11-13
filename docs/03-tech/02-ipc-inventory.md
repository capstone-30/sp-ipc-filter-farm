# 사용한 IPC 목록

필수·선택 범위는 [요구사항](../01-func/01-requirements.md#범위), 선택 테이블은 [ipc.c](../../src/ipc/ipc.c)를 따른다.

## 데이터 통신 백엔드

| IPC | 전달 방식 | 구현 |
| --- | --- | --- |
| pipe | 실행기가 클라별 요청·응답 FD를 상속한다. | [실행기](../../src/pipe_run.c) · [백엔드](../../src/ipc/ipc_pipe.c) |
| sysv_mq | 요청·응답 큐를 분리하고 최대 8,000B 청크를 재조립한다. | [송수신](../../src/ipc/ipc_sysv_mq.c) · [수명](../../src/ipc/mq_lifecycle.c) |
| sysv_shm | 클라별 요청·응답 링을 SysV SEM으로 동기화한다. | [백엔드](../../src/ipc/ipc_sysv_shm.c) |
| fifo | `<key>.req.<id>`·`<key>.resp.<id>`로 송수신한다. | [백엔드](../../src/ipc/ipc_fifo.c) |
| posix_shm | POSIX SHM 링을 프로세스 공유 sem_t로 동기화한다. | [백엔드](../../src/ipc/ipc_posix_shm.c) |

두 SHM은 [공용 링](../../src/ipc/shm_ring.c)을 사용한다. API·레이아웃 원본은 [프로토콜](03-protocol.md#데이터와-정합성)에 연결한다.

## 동기화와 제어

- [meta의 fcntl 잠금](../../src/ipc/ipc_meta.c)은 서버·ID 점유와 [수명](01-architecture.md#채널-수명)을 관리한다.
- [큐의 mutex·condvar](../../src/common/bqueue.c)는 jobs/done을 보호하며, 프로세스 내부 세마포어는 동시 작업·inflight를 제한한다.
- [시그널 처리](../../src/common/app.c)는 [유한 대기·종료 계약](01-architecture.md#종료와-유한-대기)을 따른다.
