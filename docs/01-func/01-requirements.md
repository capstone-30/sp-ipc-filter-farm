# 요구사항 — IPC Filter Farm

[README](../../README.md) · [기술 구조](../03-tech/01-architecture.md) · [테스트 명세](../04-test/README.md)

## 목적

[과제 요약](../assignment.md)에 맞춰 IPC 기초 성능을 측정하고, 이미지 필터 응용의 IPC 선택을 예측·검증·개선한다. 원문 조건과 양식은 [과제 안내문](../assignment-guide.md)·[최종보고서 서식](../final-report-template.md)에 보존하며, 현재 실행법은 [README](../../README.md#사용-방법)를 따른다.

## 범위

- 3인 팀 필수 비교는 익명 `pipe` / `sysv_mq` / `sysv_shm`+SEM이다(`fifo`·`posix_shm`은 선택 확장, 팀 규모 예외는 [과제 조건](../assignment.md#일정과-팀-구성)). 구현된 IPC와 코드 위치는 [IPC 목록](../03-tech/02-ipc-inventory.md)에서 관리한다.

| 구분 | 데이터·동시성 |
| --- | --- |
| 1단계 기초 벤치 | 필터 없는 바이트 왕복·처리량. 64B·1KiB·16KiB·256KiB·1MiB, 클라이언트 1/2/4/8 |
| 2단계 이미지 응용 | 서버 1·클라이언트 1~4, PPM P6 3채널, GRAY/BLUR/EDGE, 유계 링·inflight |

- 제외: 추가 비교 백엔드(POSIX MQ·Unix socket·eventfd 등), 원격 IPC, 동적 슬롯/경쟁 프로토콜, 재전송/응답 순서 보장, PPM 외 포맷/알파 채널, SIMD/멀티코어 필터, 온라인 백분위 근사, 벤치 자동 회귀 판정.

## 실행 인터페이스

구현 상태는 [기술 구조](../03-tech/01-architecture.md), 검증 판정은 [테스트 명세](../04-test/README.md)에서 관리한다.

```bash
./server --ipc=<name> [--team=1] [--workers=N] [--size=512] [--depth=1] [--key=/tmp/ipcff.key] \
         [--clients=N] [--warmup=0] [--count=N] [--cpu-csv=path] [--quiet] [--inplace-gray]
./client --ipc=<name> [--team=1] --id=<1..4> --filter=gray|blur|edge \
         --count=N [--size=512] [--depth=1] [--key=/tmp/ipcff.key] [--no-save] \
         [--warmup=0] [--csv=path] [--quiet]
```

- 팀 구성: 1팀·3명(`--team=1..99`, 기본 1). SysV key는 `60000+10×team+용도`(MQ 요청/응답=0/1, SHM/SEM=2/3), POSIX SHM 이름은 `/osproject_<team>_shm`이다. `--key`는 잠금·소유 기록 경로이며 숫자 key를 바꾸지 않는다. 같은 팀의 MQ/SHM 서버 중복 실행 시 기존 자원을 보존하고 실패한다.
- 공통 제약: `--ipc/team/size/depth/key`와 `payload_bytes/slot_count/warmup`은 양측 일치 필수(불일치 시 즉시 오류). `workers` 기본 2, `count/workers>0`, `size=1..2048`, `depth=1..16`, `clients=1..N`(ID `1..N`). seq는 워밍업 `0..warmup-1`, 측정 `warmup..warmup+count-1`(`warmup+count ≤ UINT32_MAX`).
- `pipe_run [--basic] --clients=N [--output=dir] [공통 옵션]`: 익명 파이프를 생성해 서버·클라를 `fork/exec`한다(`--filter=gray,edge`처럼 ID순 지정, 상속 FD 없이 `--ipc=pipe` 직접 실행 시 거부). `--output`은 프로세스별 로그·클라 CSV·`<csv>.cpu.csv`를 저장하고, `warmup≥1`이면 서버 자원 CSV도 자동 생성한다.
- 기초 벤치(`basic_server`/`basic_client`): 필터 없이 echo하며 `--bytes=64..1048576`, 클라 `1..8`, 처리량 `W=4`(지연 실험 `W=1`)를 사용한다. 데이터 `B`바이트 외 왕복 48B 헤더와 MQ 청크별 20B·`mtype` 비용을 별도 기록하며 전 바이트를 전송·검증한다. 8클라 잠금 영역과 겹치지 않게 meta 데이터 offset=16을 사용한다(구버전과 혼용 금지).
- 계측·개선 옵션: `--quiet`는 진행률 출력만 끄고 stats 스레드는 유지한다. 서버 `--cpu-csv`는 `clients/count/warmup≥1` 필수다. `--inplace-gray`는 GRAY에서만 입력 버퍼를 출력으로 재사용하는 단일 요인 실험 옵션이다(BLUR/EDGE는 분리 버퍼 유지).

## 워크로드와 예측

- 페이로드: 요청·응답 각 `3×size²` B + 헤더 48B.
- 부하 패턴: N:1 요청·개별 응답, 클라당 최대 `W`개 파이프라인 폐루프 포화 부하(요청 빈도 = 전송 건수/측정 시간).
- 서버 큐·버퍼: 작업당 `job`/`done` 큐 enqueue·dequeue 각 1회(생산:소비 1:1, 상태 쓰기 동반). 입력 원본은 읽기 전용, 결과는 작업별 독립 버퍼를 사용한다.
- 지표·예측: 주 지표는 `images/s`, 보조 지표는 `p99 RTT`와 자원 사용량이다. [원본 과제 조건](../assignment.md#필수-수행-순서)에 따라 응용 구현·측정 전 예측 원문을 보존하고, 이후 수정은 내용과 이유를 구분한다([예측 기록](../03-tech/01-architecture.md#예측-기록)).

## 측정 공정성

규칙 위반 시 비교 측정 전체를 무효로 처리한다. 측정값은 대기·상대 확인·파싱·복사·체크섬을 포함한 구현 비용이다.

| ID | 규칙 |
| --- | --- |
| R1 | 전 백엔드 동일 48B `msg_hdr_t` 사용(1단계는 지정 바이트 전송 + 제어 비용 별도 명시) |
| R2 | 모든 수신 경로에서 페이로드 체크섬 계산·대조 |
| R3 | 입력 이미지는 측정 전 메모리 적재·재사용, 측정 중 파일 I/O 금지(경계 `/proc/self/status` 읽기 간섭은 별도 명시) |
| R4 | `--no-save` 지원 및 벤치 필수 적용 |
| R5 | 시간 측정은 `CLOCK_MONOTONIC` 사용(`CLOCK_REALTIME`은 `sem_timedwait` deadline 전용) |
| R6 | 평균·p50/p95/p99/max 동시 보고 |
| R7 | 응용 내 백엔드별 분기 금지(이름은 `src/ipc/ipc.c` 테이블에 한정, usage 등 예외만 `/* R7-OK */`) |

벤치 시 진행률 출력을 끄고 stats 스레드는 유지한다. 양 단계 모두 CPU(`user/sys`), 컨텍스트 스위치(`nvcsw/nivcsw`), 프로세스별 `VmRSS`, 지연(RTT), 처리량(1단계 `MB/s`·`messages/s`, 2단계 `images/s`)을 기록한다([측정과 집계](../03-tech/01-architecture.md#측정과-집계)).

## 완료 기준

1. 경고 0 빌드 및 필수 3백엔드 loopback 통과.
2. 양측 작업 스레드 ≥3, mutex/condvar/semaphore 사용, 2클라×1,000장 비순차 seq 전건 매칭(유실·중복·체크섬 오류·혼입 0).
3. 필수 3백엔드 단위→통합→장애 통과, SIGINT·상대 종료 시 무한 대기·누수·재시작 잔존 자원 0.
4. S1~S5 CSV 5개·원자료·환경 기록·그래프 4종(페이로드–지연 p50 실선/p99 점선, 페이로드–처리량, 클라 수–처리량, S1 512² user/sys CPU 스택 막대; 앞 2종 x축 로그).
5. `sysv_shm` 2클라 이종 필터 동시 실행 전후 이미지, 필수 3종 데모 통계, 별도 `strace` 진단 비교([검증 전략](../04-test/01-verification.md#검증-전략)).
6. 1단계 전 크기×클라 수×필수 IPC 원자료·자원 계측·그래프 완비.
7. 예측 원문·시점과 구현 이력 구분, 단일 요인 원인 실험, 최소 1회 설계 개선 전후 비교·메모리 효과·IPC 선택 근거 확보.
8. [제출 요건](../assignment.md#제출물과-보고서)에 맞춘 보고서·tar·PPT·영상 및 AI 오류 검증 사례 2건 이상 확보.

## 통신 계약

필터 연산·메시지 검증·자원 보호 상세는 [IPC 프로토콜](../03-tech/03-protocol.md)을 따른다.

[분류 기준](../../README.md#문서-분류-기준) · [기술 명세](../03-tech/README.md) · [테스트 명세](../04-test/README.md)
