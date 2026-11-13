# 테스트 명세 — IPC Filter Farm

완료 기준: [요구사항](../01-func/01-requirements.md) · 구조·남은 작업: [기술 구조](../03-tech/01-architecture.md) · 실행: [루트 README](../../README.md#테스트)

## 검증 전략

CI는 Linux에서 경고 없는 전체 빌드·헤더 검사와 공통·IPC·MQ·SHM·응용·pipe 회귀, 벤치 smoke 및 제출 도구 검사를 수행하도록 구성한다. 정식 성능 측정은 [README](../../README.md#테스트) 절차와 구분한다. CI 설정의 추가를 실제 실행 통과로 인정하지 않는다.

### 현재 검증 판정

메타/FIFO·BYE·패키지 수정본은 macOS만 확인했다. 아래 Linux 검사·성능 판정은 각 로그·스냅샷의 수정 전 리비전에 한정하며 수정본으로 확장하지 않는다. Linux CI·회귀 실행 후 갱신한다.

| 환경 | 판정 | 범위 |
| --- | --- | --- |
| macOS 공통·도구 | 통과 | 공통 모듈·헤더·GRAY 동등성·IPC 유틸·옵션·집계/그래프 fixture·문법·문서·빌드 dry-run·초안 tar를 확인했다([로그](../../tests/logs/macos-assignment-contract.log)). |
| macOS 수정본 | 통과 | 메타/FIFO 파일 보호·실패 회수·장애·취소·재시작을 네이티브와 ASan/UBSan으로 확인했다. 공통·옵션·집계·패키지·제출 검사도 통과했다. Linux BYE 오류·전체 응용은 미실행이다. |
| 제출 검사 | 통과 | 합성 S1 전체 조합 통과와 작은 크기·IPC 누락 12종 및 기존 대형 범위만 있는 자료의 거절을 확인했다. |
| Linux 빌드·회귀 | 수정 전 통과 | 전체·6개 개별 빌드·헤더·단위·팀·IPC·응용 5종·SHM depth=1/4/16·장애·정리·벤치 smoke를 확인했다. 재시작 검사·정리 추가 5회도 통과했고 잔존 IPC·프로세스는 없었다. |
| 기초 산출물 | 검증 통과 | [360회·135만 표본과 검증 한계](../basic-measurement.md#검증-판정과-남은-확인)를 따른다. |
| 응용·개선·S5 산출물 | 검증 통과 | [측정 결과](../app-measurement.md#검증-판정과-남은-확인)와 [S5 3쌍 비교](../app-measurement.md#s5-mq-큐-용량)를 따른다. |
| 데모·R3 | 정합성·정정 범위 통과 | [이미지 대조·9개 CPU 구간·제어 I/O·R3 측정 후 정정](../diagnostics-measurement.md)을 따른다. |

Linux 회귀 근거는 [linux-restart-fix.log](../../tests/logs/linux-restart-fix.log), 최신 macOS 근거는 [macos-risk-fixes.log](../../tests/logs/macos-risk-fixes.log)다. Linux 소스·개별 로그·해시는 로컬 `bench/results/linux-restart-fix-20261113/`에 보존했다. `startup_metadata`는 잔존 ready와 게시 PID·잠금 PID 불일치를 순서 고정으로 재현한 검사이며, 최초 FIFO 실패의 실행 순서를 소급한 근거는 아니다.

### 검증 항목과 판정 기준

| 항목 | 방법·판정 |
| --- | --- |
| 과제·제출 | [완료 기준](../01-func/01-requirements.md#완료-기준)·[제출 요건](../assignment.md#제출물과-보고서) 대조, tar 해제 후 Ubuntu+gcc 빌드·실행·측정 재현 |
| 빌드·계약 | 빌드·헤더 검사 종료 0, 정적 assert(헤더 48B, 청크 최대 8,020B, 링 정렬) |
| 팀 식별 | 범위·중복 옵션, 팀 불일치 시 ID 미소비, 동일 팀 타 경로 충돌 시 기존 서버 보호, 1·2팀 동시 왕복·정리 |
| 단위 | `test_bqueue`(4P×4C 100만 건), `test_proto`(체크섬), `test_ppm`(왕복·오류 거절·첫 바이트 보존), `test_filter`(3×3 수기 배열 대조), `test_timer`(nearest-rank 백분위·처리량·할당 실패 중단) |
| loopback | 12KB/512KB/12MB (`fork` 분리 실행) |
| `pipe` / `fifo` 장애 | 불필요 FD 폐쇄, 부분 전송·동시 클라·EOF/`EPIPE`·`SIGINT`·자식 회수(`pipe`), 클라/서버 `SIGKILL`·ID 격리·`ECONNRESET`·`ECANCELED`·재시작 잔존 0(`fifo`) |
| `sysv_mq` 장애 | 역순·교차 seq, 불량 청크 11종·`W` 한도, 클라/서버 `SIGKILL`·소유 기반 재시작·충돌 시 타 큐 보존, 30초 정체 퇴역 및 큐·경로 소멸 |
| SHM 장애·정리 | `sysv_shm`/`posix_shm` `depth=1/4/16` 2클라×1,000건 역순 응답·홀수 stride·전건 매칭, `SIGKILL` 격리·회수, 실행 중 `clean` 거절·타 객체 보존, POSIX `sem_destroy` 횟수·용량 오류 회수·이름 소멸 |
| 응용·통합 | 역순 허용·중복/변조 거절, 256² 전후 이미지·CSV·`BYE`, 불량 요청 8종 `MSG_ERR`, 512² 2클라×1,000장(`depth=4`)·BLUR(`depth=16`) seq=0..999 전건 매칭 |
| 스레드·자원 | `workers=2` 실행 중 `/proc/<pid>/task` 서버 5·클라 4 확인; `SIGINT` 전후 본인 `ipcs -q -m -s`·POSIX SHM 이름·key 경로 잔존 0 확인 |

| 공정성 규칙 | 확인 방법 |
| --- | --- |
| R1/R2 | `test_proto`, 불량 체크섬 거절 및 전 수신 경로 체크섬 검증 |
| R3 | 확장 `TRACE=io`에서 프로세스별 `getrusage` 경계 사이 일반 파일 I/O를 확인한다(`pread64/pwrite64`, FD 경로·스레드 소속 포함). 이미지·CSV, 제어 메타데이터와 경계 `VmRSS` 읽기를 구분하며 일부 부재로 전체 통과를 대신하지 않는다. |
| R4/R5 | `run_bench.sh`의 `--no-save` 적용 및 `CLOCK_REALTIME`의 `sem_timedwait` 전용 사용 확인 |
| R6/R7 | 집계 CSV 백분위 열 확인; 응용 내 `R7-OK` 외 백엔드명 출현 0 확인 |
