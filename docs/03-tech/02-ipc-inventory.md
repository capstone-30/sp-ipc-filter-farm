# 사용한 IPC 목록

현재 소스에 구현된 통신 백엔드 5종과 동기화 수단이다([백엔드 선택 테이블](../../src/ipc/ipc.c)). 필수 범위는 [요구사항](../01-func/01-requirements.md#범위), 실행법은 [README](../../README.md#사용-방법), 검증 상태는 [테스트 명세](../04-test/01-verification.md#현재-검증-판정)를 따른다.

## 데이터 통신 백엔드

| IPC / 이름 | 구분 | 프로젝트 사용 방식 | 주요 API | 구현 위치 |
| --- | --- | --- | --- | --- |
| 익명 파이프 / `pipe` | 필수 | 실행기가 클라별 요청·응답 파이프 FD를 생성해 서버·클라에 상속하고 바이트 스트림으로 송수신한다. | `pipe`, `fork`, `execv`, `read`, `write`, `poll`, `close` | [실행기](../../src/pipe_run.c), [백엔드](../../src/ipc/ipc_pipe.c) |
| SysV 메시지 큐 / `sysv_mq` | 필수 | 요청(`mtype=1`)·응답(`mtype=client_id`) 큐를 분리하고 최대 8,000B 청크로 분할·재조립한다. | `msgget`, `msgsnd`, `msgrcv`, `msgctl` | [송수신](../../src/ipc/ipc_sysv_mq.c), [수명](../../src/ipc/mq_lifecycle.c), [헤더](../../src/ipc/ipc_sysv_mq.h) |
| SysV SHM + SEM / `sysv_shm` | 필수 | 클라별 요청·응답 링에 복사하고 SysV 세마포어로 빈/채워진 슬롯을 동기화한다. | `shmget`, `shmat`, `shmdt`, `shmctl`, `semget`, `semop`, `semctl` | [백엔드](../../src/ipc/ipc_sysv_shm.c), [헤더](../../src/ipc/ipc_sysv_shm.h) |
| 이름 있는 파이프 / `fifo` | 선택 | `<key>.req.<id>`·`<key>.resp.<id>` 경로로 독립 프로세스 간 송수신한다(필수 `pipe` 대체 아님). | `mkfifo`, `open`, `read`, `write`, `poll`, `close`, `unlink` | [백엔드](../../src/ipc/ipc_fifo.c) |
| POSIX SHM + SEM / `posix_shm` | 선택 | POSIX SHM 매핑과 공유 헤더 내 `sem_t`(`pshared=1`)로 클라별 링을 동기화한다. | `shm_open`, `ftruncate`, `posix_fallocate`, `mmap`, `munmap`, `shm_unlink`, `sem_init`, `sem_trywait`, `sem_timedwait`, `sem_post`, `sem_destroy` | [백엔드](../../src/ipc/ipc_posix_shm.c), [헤더](../../src/ipc/ipc_posix_shm.h) |

두 SHM 백엔드는 [공용 링](../../src/ipc/shm_ring.c)을 공유하며 슬롯 획득→복사→게시 후 로컬 버퍼로 다시 복사한다. 식별자 규칙은 [실행 인터페이스](../01-func/01-requirements.md#실행-인터페이스)를 따른다.

## 공통 프로세스 간 제어

| 수단 | 용도 | 주요 API | 구현 위치 |
| --- | --- | --- | --- |
| 파일 레코드 잠금 | `<key>.meta`에서 서버·클라 ID 중복 점유 방지 및 상대 수명 확인([채널 수명](01-architecture.md#채널-수명)) | `fcntl(F_SETLK, F_GETLK)` | [ipc_meta.c](../../src/ipc/ipc_meta.c) |
| 시그널 | `SIGINT` 종료 요청 및 `pipe_run` 자식 회수([종료 절차](01-architecture.md#종료와-유한-대기)) | `pthread_sigmask`, `sigtimedwait`, `kill` | [app.c](../../src/common/app.c), [pipe_run.c](../../src/pipe_run.c) |

## 프로세스 내부 스레드 동기화

| 수단 | 용도 | 구현 위치 |
| --- | --- | --- |
| pthread mutex·condvar | 서버 `jobs`/`done` 유계 큐, 클라 측정 상태·송신 대기, 백엔드 송수신 보호 | [bqueue.c](../../src/common/bqueue.c), [client.c](../../src/client.c), [ipc.h](../../src/ipc/ipc.h) |
| 이름 없는 세마포어(`pshared=0`) | 클라 `inflight` 요청 수 및 서버 동시 작업 수 제한 | [client.c](../../src/client.c), [server.c](../../src/server.c) |
