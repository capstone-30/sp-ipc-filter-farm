# 기술 구조 및 설계 — IPC Filter Farm

[요구사항](../01-func/01-requirements.md)을 구현한다. 실행은 [README](../../README.md), 판정은 [테스트 명세](../04-test/01-verification.md)를 따른다.

## 아키텍처와 경계

### 응용 구조

- 서버는 `receiver → jobs → worker N → done → responder`로 처리한다. `jobs/done`은 mutex·condvar 유계 링이며, 작업 세마포어 상한은 `SLOT_COUNT×depth`다. 지정한 모든 클라의 `BYE`를 받으면 종료하고, `--clients` 미지정 시 `SIGINT`까지 기다린다.
- 클라는 `sender/receiver/stats`를 사용한다. `sender`는 `inflight`를 획득해 송신하고, `receiver`는 응답 검증·RTT 기록 후 반환한다. `stats`는 벤치에서도 유지하되 진행 출력을 끈다.
- 작업은 큐를 통해 소유권을 이전한다. 입력은 읽기 전용이고 출력은 작업별 독립 버퍼다. `--inplace-gray`만 GRAY 입력을 재사용한다. 백엔드 전용 수신 스레드는 없다.

### 백엔드

[IPC 목록](02-ipc-inventory.md)이 구현 위치를, [프로토콜](03-protocol.md)이 데이터·검증 계약을 관리한다. MQ는 ID당 최대 `W`개를 재조립하고 잠금 소실·BYE·30초 무청크 시 폐기·퇴역한다. SHM은 슬롯 획득→복사→게시 후 수신 로컬 버퍼로 복사한다. `send/recv` 밖 SHM 포인터 보존·`SEM_UNDO`·임의 토큰 복구는 금지한다.

### 종료와 유한 대기

스레드 생성 전 `SIGINT`를 차단하고 main의 100ms `sigtimedwait`로 감시한다. 종료 순서는 `g_stop → bq_close → join → 잔여 작업 해제 → 서버 destroy+close / 클라 close`다. 정상 클라는 전 seq 검증 후 `BYE`를 한 번 보내며 오류·SIGINT에서는 생략한다.

| 대기 대상 | 방식 |
| --- | --- |
| pipe/FIFO | 비차단 I/O와 최대 100ms poll, 부분 전송·EOF·EPIPE 처리 |
| SysV MQ/SEM | IPC_NOWAIT 후 1ms 재시도 |
| POSIX SEM | 탐색 sem_trywait, 특정 링 최대 100ms sem_timedwait |
| SHM 요청 | ID별 round-robin 탐색 후 모두 비었을 때만 1ms 대기 |
| inflight/stats | 100ms 단위 취소 확인 |

송수신은 `EINTR/EAGAIN/ETIMEDOUT`을 재시도하되 중단 시 `ECANCELED`를 반환한다. timeout만으로 사망을 판정하지 않는다. `pipe_run`은 부모 실패 시 SIGINT 후 5초 뒤 SIGKILL로 자식을 회수한다.

### 채널 수명

[meta](../../src/ipc/ipc_meta.h)는 프로세스당 단일 FD의 배타 `fcntl` 레코드 잠금으로 서버·ID 수명을 관리한다.

- 서버는 잠금→`ready=0`→IPC 초기화→설정·PID 게시→`ready=1` 순서로 준비한다.
- 클라는 최대 5초 동안 ready와 게시 PID·잠금 소유 PID의 일치를 기다린다. 설정 불일치는 ID 확보 전 거절하고 연결 후 100ms마다 상대 잠금을 확인한다.
- 잠금 소실만 사망으로 판정한다. 죽은 클라 ID는 퇴역하며 재사용하지 않고 다른 ID는 계속 처리한다. 서버가 죽으면 클라는 `ECONNRESET`으로 종료한다.
- 재시작·정리는 전 잠금 소실과 소유 기록(생성 식별자·UID·inode) 일치 시에만 회수한다. 실행 중이거나 미소유 자원은 보존한다. meta는 심볼릭·다중 하드 링크와 비정규 파일을 거절하고, FIFO는 호출자 소유만 열며 초기화 실패 시 이번 실행에서 만든 것만 회수한다.

## 소스 구조

| 경로 | 역할 |
| --- | --- |
| `src/` | 서버·클라·pipe 실행기·정리 진입점 |
| `src/common/` | 프로토콜·큐·PPM·필터·계측 |
| `src/ipc/` | 백엔드·메타데이터·소유권 |
| `tests/` | 단위·통합·장애 검사와 logs/ |
| `bench/` | 측정·집계·그래프·패키징 도구와 results/ |
| `docs/` / `submission/` / `build/` | 문서 / 제출물 / 개별 IPC 빌드 |

## 벤치 설계

### 1단계 기초 측정

[범위](../01-func/01-requirements.md#범위)의 전 조합을 지연 `W=1`·처리량 `W=4`로 측정한다.

### 2단계 응용 측정

| ID | 변수 | 고정 조건·목적 |
| --- | --- | --- |
| S1 | 한 변 64/80/96/104/256/512/1024/2048 | 클라 1·GRAY·W=1, 작은 페이로드 포함 역전 지점 |
| S2 | 클라 1/2/4 | 512²·GRAY·W=1, 확장성 |
| S3 | GRAY/BLUR/EDGE | 512²·클라 2·W=1, 연산 비용 |
| S4 | W=1/4/16 | 512²·클라 1·GRAY, 파이프라인 |
| S5 | kernel.msgmnb 기본/확대 | MQ·1024², 백프레셔 |

### 측정과 집계

워밍업 100회·클라당 1,000회·3반복의 실행별 지표 중앙값을 사용한다. 명령은 [README](../../README.md#기초-측정--예측-작성--응용-측정)를 따른다.

- 서버는 ID별 마지막 워밍업 응답을 보류하고 전 ID 완료 시 시작 RSS→getrusage를 기록해 일괄 송신한다. 클라는 워밍업 전건 수신 후 같은 순서로 기록하고 측정을 시작한다. 종료는 서버의 마지막 측정 응답 송신·클라의 마지막 검증 직후 getrusage→RSS 순서다.
- RTT는 송신 직전부터 체크섬·seq 검증 직후까지다. 사전 할당 표본을 종료 후 정렬해 실행 내 전 클라의 nearest-rank p50/p95/p99·max·mean을 계산한다.
- 처리량은 성공 건수 또는 단방향 유효 바이트를 `max(last_recv_ns)-min(first_send_ns)`로 나눈다(MB=1,000,000B). CPU(user/sys)·컨텍스트 스위치(nvcsw/nivcsw)·VmRSS를 기록한다. CPU는 서버·클라 누적 시간의 합계다. RSS는 경계 시점 합계로 공유 페이지 중복을 포함할 수 있으며 최대·물리 메모리가 아니다.
- 원시 RTT·자원 sidecar와 집계 형식은 [집계 코드](../../bench/aggregate.c)를 따른다. 원자료는 `raw/`, 환경(CPU·메모리·OS·gcc·가상화·workers·affinity·큐 용량)은 `environment.txt`에 보존한다.

### 예측 기록

[예측 문서](../ipc-prediction.md)의 원문과 이후 수정 내용·이유를 구분한다. 응용 구현·측정 전 작성은 [과제 조건](../assignment.md#필수-수행-순서)이며 실제 시점을 소급하지 않는다. `freeze_prediction.sh`의 동결본·해시는 선택 사항이다.

### 원인 실험과 개선

락 범위·버퍼 깊이·복사 횟수 중 한 요인만 바꿔 동일 조건의 지연·처리량·CPU·전환·RSS를 비교한다. 최소 1개 개선의 효과·부작용과 근거 CSV·소스 스냅샷을 보존한다.

## 남은 작업과 리스크

| 대기 항목 | 이유·재개 조건 |
| --- | --- |
| 수정본 Linux 검증 | 메타/FIFO·BYE·패키지 수정 후 macOS만 확인했다. Linux에서 전체 회귀 후 [판정](../04-test/01-verification.md#현재-검증-판정)을 갱신한다. |
| 최종 보고서·PPT·영상·AI 대화 캡처 | 팀원·담당·제출 일시와 최종 자료가 필요하다. 확인 후 [서식](../final-report-template.md)에 반영한다. |
| 제출 패키지 | 최종 자료 확보 후 새 경로에 해제해 Ubuntu+gcc로 빌드·실행·측정을 재현한다. |
| 재측정 환경 | 새 환경에서는 SHM 용량·메모리·S5 확대 권한을 확인한다. BENCH_TIMEOUT으로 제한하고 반복 축소를 환경에 기록한다. |
