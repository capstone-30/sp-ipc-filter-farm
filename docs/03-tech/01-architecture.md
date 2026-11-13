# 기술 구조 및 설계 — IPC Filter Farm

[요구사항](../01-func/01-requirements.md)을 구현하는 아키텍처·소스 구조·벤치 설계와 남은 작업을 관리한다. 검증 판정은 [테스트 명세](../04-test/01-verification.md), 사용법은 [README](../../README.md)를 따른다.

## 아키텍처와 경계

### 응용 구조

- 서버: `main`(인자·채널·시그널·정리) + `receiver` 1(수신→`jobs`) + `worker` N(필터→`done`) + `responder` 1(대상 클라 송신). 작업 세마포어 상한은 `SLOT_COUNT×depth`다. `--clients`의 전 ID `BYE` 수신 시 종료하며 미지정 시 `SIGINT`까지 대기한다.
- 클라: `main` + `sender` 1(`inflight` 획득→송신 시각 기록→전송) + `receiver` 1(검증→RTT 누적→`inflight` 반환) + `stats` 1(데모 1초 진행률, 벤치 출력 억제). `inflight`는 `sem_init(&inflight, 0, W)`이며, 샘플은 사전 할당 배열에 누적 후 종료 시 `qsort`로 계산한다. 완료 시 `client-<id>-input.ppm`·`client-<id>-output.ppm`을 저장한다(`--no-save` 제외).
- 큐 동기화: `jobs`/`done`은 고정 링 + mutex/condvar(`while` 조건 대기, `close` 시 양쪽 broadcast)를 사용한다. 백엔드 전용 수신 스레드는 두지 않는다.

### 백엔드

- 공통 식별·소유권: [실행 인터페이스](../01-func/01-requirements.md#실행-인터페이스)와 [IPC 목록](02-ipc-inventory.md)을 따른다. `ipc_config_t.team`을 meta에 게시해 ID 확보 전 대조한다. `<key>.mq`·`<key>.shm`·`<key>.pshm`에 생성 식별자·UID·inode를 기록하고, 재시작·[`clean-ipc`](../../README.md#사용-방법)는 전 잠금 소실과 소유 기록 일치를 확인한 뒤 회수한다(미소유·실행 중 자원은 보존 후 오류 종료).
- `pipe`: 방향별 단일 writer, 부분 `read/write`·`poll`·`SIGPIPE` 처리. 부모 실패 시 `SIGINT` 후 5초 뒤 `SIGKILL`로 전 자식을 회수한다.
- `fifo`: ID별 부분 상태 격리, `open`/`ENXIO`/EOF 유한 대기·상대 확인, 사망 ID의 부분 메시지·`EPIPE` 응답만 폐기.
- `sysv_mq` ([계약](../../src/ipc/ipc_sysv_mq.h)): ID 범위 → 수신 길이 → `bytes≤8000` → `chunk_idx<chunk_cnt` → 전체 `≤48+MAX_PAYLOAD` → 0번 헤더 ID/seq 일치 순으로 검증한다. `(client_id, seq)` 재조립은 ID당 최대 `W`개이며, 잠금 소실·`BYE`·30초 무청크 시 즉시 폐기·퇴역(`used=2`) 처리한다. 송수신 100ms마다 상대 PID·퇴역·미완성 슬롯을 점검한다.
- `sysv_shm` / `posix_shm` ([SysV](../../src/ipc/ipc_sysv_shm.h) · [POSIX](../../src/ipc/ipc_posix_shm.h)): 할당 전 `slot_bytes`·전체 크기 overflow·용량을 검사한다(`stride = align(48+slot_bytes)`). 단조 `head/tail`은 프로세스 로컬 상태(`% W`)이며, `req_empty` 획득→복사→`req_full` 게시 및 `req_full` 획득→로컬 복사→`req_empty` 반환 순서로 동작한다(`resp_*` 동일, `send/recv` 밖 SHM 포인터 보존·`SEM_UNDO`·임의 토큰 복구 금지). POSIX SHM은 `ftruncate`→`posix_fallocate`로 용량을 확보하고, 전 ID 잠금 획득 시에만 `sem_destroy`를 호출한다.

### 종료와 유한 대기

1. 스레드 생성 전 `pthread_sigmask`로 `SIGINT`를 차단하고 `main`에서 100ms `sigtimedwait`로 감시한다(비동기 핸들러·`SIGUSR1` 금지).
2. `SIGINT`/치명적 오류 시 atomic `g_stop` 설정 → `jobs`/`done` `bq_close` → 전 스레드 `join` → 잔여 큐 [해제](../../src/common/bqueue.h) → 서버 `destroy+close` / 클라 `close`([계약](../../src/ipc/ipc.h)) 순으로 정리한다.
3. 정상 완료 시 `sender`가 ≤100ms 간격으로 전 seq 검증 완료를 확인한 뒤 `BYE` 1회 송신 후 종료한다(`SIGINT`/오류 시 `BYE` 생략).

| 대기 대상 | 방식 |
| --- | --- |
| FIFO | `O_NONBLOCK` + 최대 100ms `poll` (`SIGPIPE` 무시, `EPIPE` 오류) |
| SysV MQ/SEM | `IPC_NOWAIT` + 재시도 간 1ms `nanosleep` |
| POSIX SEM | 탐색 `sem_trywait`, 특정 링 최대 100ms `sem_timedwait` |
| SHM 요청 탐색 | 전 ID `req_full` round-robin try 후 모두 비었을 때만 1ms 대기 |
| `inflight` / `stats` | 100ms 단위 재시도 / 대기 분할 |

`send/recv`는 완성까지 `EINTR/EAGAIN/ETIMEDOUT`을 재시도하되 `g_stop`이면 `-1/ECANCELED`를 반환한다(timeout 자체는 사망이 아니며 상대 잠금 확인 후 재시도).

### 채널 수명

`<key>.meta`([구현](../../src/ipc/ipc_meta.c)·[헤더](../../src/ipc/ipc_meta.h))의 프로세스당 단일 FD 배타 `fcntl` 레코드 잠금으로 서버·클라 ID 수명을 관리한다.

- 서버: 잠금 → `ready=0` → IPC 초기화 → `server_pid/backend/size/depth` 기록 후 `ready=1`.
- 클라: `ready=1`과 서버 잠금 소유 PID·게시 PID의 일치까지 최대 5초 대기한다. 재시작 전의 잔존 `ready`는 준비 완료로 인정하지 않는다. 준비된 서버의 설정 불일치는 `EINVAL`로 거절하며, 연결 후에는 100ms 대기마다 `F_GETLK`로 상대 잠금을 확인한다.
- 사망 처리: 잠금 소실만 사망으로 판정한다. 클라 사망 시 서버는 해당 ID만 퇴역·폐기하고 타 ID 처리를 계속하며, 서버 잠금 소실 시 클라는 `ECONNRESET`으로 종료한다. 클라 ID는 서버 실행당 1회만 사용한다.

## 소스 구조

```text
src/                 server, client, pipe_run, clean_ipc 진입점
  common/            프로토콜·유계 큐·PPM·필터·타이머·자원 계측
  ipc/               IPC 백엔드·메타데이터·소유권 관리
tests/               단위·통합·장애 검사 및 logs/
bench/               측정·집계·그래프·예측 동결·패키징 도구와 results/
docs/                명세·과제 원문·보고서·환경 문서
submission/          제출 산출물
build/               개별 IPC 빌드 산출물
```

| 영역 | 주요 파일 |
| --- | --- |
| 응용·공통 | [server.c](../../src/server.c), [client.c](../../src/client.c), [app.h](../../src/common/app.h), `src/common/`([단일 원본 헤더](03-protocol.md#데이터와-정합성)) |
| IPC·실행 | [ipc.c](../../src/ipc/ipc.c), [ipc_meta.h](../../src/ipc/ipc_meta.h), [pipe_run.c](../../src/pipe_run.c), [resource.c](../../src/common/resource.c) |
| 검증·빌드 | `tests/`, `tests/logs/`, [Makefile](../../Makefile), `bench/` |

## 벤치 설계

### 1단계 기초 측정

[기초 벤치 범위](../01-func/01-requirements.md#범위)의 전 조합을 측정한다(RTT 실험 `W=1`, 처리량 실험 `W=4`). 결과와 그래프 4종은 `bench/results/basic/`에 저장한다.

### 2단계 응용 측정

`run_bench.sh s5`는 전체 응용 벤치와 같은 S5 기본/확대 쌍만 실행한다. 확대 권한과 원래 `msgmnb`를 기록하고 종료·실패 시 복원한다.

| ID | 변수 | 고정 조건·목적 |
| --- | --- | --- |
| S1 | 한 변 64/80/96/104/256/512/1024/2048(12KiB~12MiB, ≤32KiB 조밀 측정) | 클라 1, GRAY, `W=1`; 백엔드 역전 지점 확인 |
| S2 | 클라 1/2/4 | 512², GRAY; 확장성·슬롯 경쟁 |
| S3 | GRAY/BLUR/EDGE | 512², 클라 2; 연산 비용 증가에 따른 IPC 차이 희석 |
| S4 | `W=1/4/16` | 512², 클라 1; 파이프라인 처리량 |
| S5 | `kernel.msgmnb` 기본/확대 | `sysv_mq`, 1024²; 큐 백프레셔(측정 후 `IPC_STAT` 확인 및 `sysctl` 원복) |

### 측정과 집계

기본 조건은 워밍업 100회·클라당 1,000회·3회 반복(중앙값 집계)이며 실행 명령은 [README](../../README.md#기초-측정--예측-작성--응용-측정), 서버 사양은 [서버 환경 기록](../04-test/02-server-environment.md)을 따른다.

- CPU·자원 경계: 서버 `responder`는 ID별 마지막 워밍업 응답 1개를 보류하다가 전 ID 워밍업 완료 시 서버 시작 RSS→`getrusage`를 기록하고 일괄 전송한다. 클라는 워밍업 전건 수신 후 시작 RSS→`getrusage`를 기록하고 측정 송신을 시작한다. 종료 시에는 서버(전 측정 응답 송신 직후)와 클라(마지막 응답 검증 직후) 모두 `getrusage`→RSS 순으로 읽어 CPU 구간에서 파일 읽기를 제외한다.
- 통계·처리량: RTT는 송신 직전~체크섬·seq 검증 직후이며, 실행별 전 클라 표본으로 nearest-rank p50/p95/p99·max·mean을 구한다(`stats_report`는 진행 표시 전용). 처리량은 `(성공 이미지 수 또는 단방향 유효 바이트) / (max(last_recv_ns) - min(first_send_ns))`로 계산한다(MB=1,000,000B).

| 파일 | 경로·열 |
| --- | --- |
| 클라 원시 샘플 | `--csv=<path>`: `client_id,seq,rtt_ns,payload_bytes` |
| 클라 자원 sidecar | `<csv>.cpu.csv`: `ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,first_send_ns,last_recv_ns` |
| 서버 자원 sidecar | `--cpu-csv=<path>`: `ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib` |
| 시나리오 집계 | `bench/results/s<N>.csv`: `backend,scenario,size,clients,filter,depth,count,p50_us,p95_us,p99_us,max_us,mean_us,mbps,imgs,ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib` |

원시 자료는 `bench/results/raw/`, 환경 정보(CPU·메모리·OS/커널·gcc·가상화·workers·taskset·`msg_qbytes`)는 `environment.txt`에 보존한다.

### 예측 기록

[원본 과제 조건](../assignment.md#필수-수행-순서)에 따라 응용 구현·측정 전에 작성한 예측 원문을 보존하고, 이후 수정한 내용과 이유를 구분한다. 측정 결과와의 대조는 검증 결과에 기록한다.

예측 문서는 [IPC 구현·성능 예측](../ipc-prediction.md)에서 관리한다. `bench/freeze_prediction.sh`는 선택적인 보관 도구이며, 동결 디렉터리와 해시는 벤치 실행·과제 제출의 필수 조건이 아니다.

### 원인 실험과 개선

실측 차이에 대해 락 범위·버퍼 깊이·복사 횟수 중 단일 요인만 변경해 동일 조건에서 지연·처리량·CPU·컨텍스트 스위치·RSS를 비교한다. 최소 1개 개선(후보: `--inplace-gray`)의 효과와 부작용, 근거 CSV·스냅샷을 보존한다.

## 남은 작업과 리스크

### 남은 검증과 제출

진단 보완에서는 `TRACE=io`에 FD 경로와 프로세스 생성, `pread64/pwrite64`를 포함해 프로세스별 CPU 경계 안의 이미지·CSV 및 제어 메타데이터 I/O를 구분한다. 기존 추적에 없는 호출을 부재로 판정하지 않는다.

[응용 측정과 개선 비교](../app-measurement.md)에서 S1~S4·S5 기본 용량 및 단일 요인 개선의 산출물을 검증했다. S5 기본/확대 3쌍의 6,000표본과 실제 큐 용량·기본값 복원을 검증했다. [데모·strace 분석](../diagnostics-measurement.md)에서 이미지·CSV I/O 부재와 제어 메타데이터·런타임 파일 접근을 구분했다. 과제 원문 확인 후 자체 R3의 범위를 명확히 했으며, 결과는 해당 구현의 전체 경로 비용으로 해석한다. 최종 보고서·PPT·영상·팀 정보와 패키지 해제 후 Ubuntu+gcc 검증은 아직 필요하다.

### 단계별 계획

| 단계 | 산출물·게이트 |
| --- | --- |
| A | 팀·환경·담당 확인, 기존 이력과 `pipe`·팀 key·8클라 기초 벤치 계약 대조 |
| B | 필수 3종 기초 벤치·개별/전체 빌드, 1단계 원자료·그래프·해석 |
| C | 워크로드 정량화·1단계 근거 예측 작성 |
| D | 응용 `pipe`·팀 key·자원 계측·출력 억제 단위→통합→장애 검증 |
| E | S1(`APP_SIZES`에 64²~32KiB 포함)~S5 측정·예측 대조·단일 요인 개선 비교 |
| F | [보고서 서식](../final-report-template.md) 기반 최종 보고서·AI 기록·재현 tar·PPT·영상 및 Ubuntu+gcc 최종 검증 |

| 리스크 | 대응 |
| --- | --- |
| 팀·일정·예측 기록 | IPC 담당자·제출 일시를 확인하고 예측 원문과 수정 내용·이유를 보존한다. |
| 장시간 MQ 측정 (R-B) | 2048²(1,573청크) 왕복 시간 실측 후 하루 초과 시 대형 페이로드만 반복 3→1회로 축소하고 `environment.txt`에 기록 |
| 권한·메모리 제한 | `sudo -v` 불가 시 S5 기본 `msgmnb`만 측정; S4(약 96MiB) 실행 전 SHM 한도 확인 |
