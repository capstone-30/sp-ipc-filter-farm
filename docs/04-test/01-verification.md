# 테스트 명세 — IPC Filter Farm

완료 기준: [요구사항](../01-func/01-requirements.md) · 구조·남은 작업: [기술 구조](../03-tech/01-architecture.md) · 실행: [루트 README](../../README.md#테스트)

## 검증 전략

CI는 Linux에서 경고 없는 전체 빌드·헤더 검사와 공통·IPC·MQ·SHM 회귀 테스트를 수행한다. 장시간 시나리오와 벤치는 [README](../../README.md#테스트) 절차로 실행한다.

### 현재 검증 판정

근거는 [로컬 검증 로그](../../tests/logs/macos-assignment-contract.log), 2026-11-12 검증한 [기초 측정 결과](../basic-measurement.md), 2026-11-13(한국 시각)의 [수정 후 Linux 검사 로그](../../tests/logs/linux-restart-fix.log)다. 이전 Linux 회귀 로그는 구버전 코드이므로 제외한다.

| 환경 | 판정 | 범위 |
| --- | --- | --- |
| macOS | 통과 | 공통 모듈(큐 100만 건), 헤더, GRAY in-place 동등성, IPC 유틸, 옵션·페이로드 검증, 집계·그래프 fixture, Bash 문법, 링크·경로, 전체·6개 개별 빌드 dry-run, 초안 tar 생성·검증 |
| macOS 제출 검사 | 통과 | `python3 tests/test_check_results.py`로 합성 자료의 전체 S1 통과, 작은 크기·IPC별 누락 12종과 기존 256² 이상 범위만 있는 자료의 거절을 확인했다. 벤치 스크립트 Bash 문법과 문서 검사도 통과했다. 변경된 기본 범위의 Linux 정식 측정은 미실행이다. |
| Linux 기초 측정 | 산출물 검증 통과 | 필수 3종의 전 실험 조합·원자료·자원 집계·중앙값·그래프·체크섬·실행 후 IPC 기록을 확인했다. 상세 근거와 한계는 [측정 결과](../basic-measurement.md#검증-판정과-남은-확인)를 따른다. |
| Linux 빌드·단위 | 통과 | Ubuntu 24.04.5·GCC 13.3.0에서 전체·필수 3종 단계별 개별 빌드 6개·헤더를 경고 없이 빌드했다. 공통 단위·팀 계약·IPC 왕복·FIFO/pipe 프로토콜·MQ 장애 검사가 통과했다. |
| Linux 응용·도구 | 통과 | pipe·FIFO·MQ·SysV/POSIX SHM 응용 통합, SysV SHM depth=1/4/16, 2클라×1,000장·스레드 수·SIGINT·정리, pipe 자식 회수, 벤치 smoke·집계 검사를 통과했다. |
| Linux SHM 장애·정리 | 통과 | SHM/POSIX 전용 장애·정리 검사와 `test_clean.sh` 추가 5회 연속 실행이 통과했다. 잔존 ready·잠금 PID 불일치·ready=0·정상 연결·취소 회귀도 통과했다. |
| 응용·개선 정식 실측 및 공정성 추적 | 미확인 | S1~S5·개선 성능 측정과 별도 `strace` 진단은 이번 검사의 범위에 포함하지 않았다. |

Linux 검사는 서버의 별도 디렉터리 `/home/student/cestudent/22/ce22b035/ipcff-restart-fix.C8B8bM/`에서 수행했다. 수정 전후 소스·개별 로그·산출물 아카이브·해시는 로컬 `bench/results/linux-restart-fix-20261113/`에 보존했다. 당시 실행한 코드·테스트의 해시가 로컬 파일과 일치함을 확인했다. 이후 S1 측정 범위와 제출 검사 변경은 위 macOS 제출 검사로 구분하며 기존 Linux 판정을 확장하지 않는다. 종료 후 MQ·SHM·SEM, 팀 1·2 POSIX SHM 이름 및 검사 프로세스 잔존은 없었다.

`startup_metadata` 회귀는 파이프로 실행 순서를 고정해 서버 잠금이 없거나 새 PID의 잠금과 이전 ready가 섞인 상태를 검사한다. 수정 전에는 잔존 ready를 준비 완료로 인정해 실패했고, 수정 후에는 게시 PID와 실제 잠금 소유 PID가 일치할 때까지 대기해 통과했다. 최초 FIFO 실패의 정확한 실행 순서를 소급하지 않으며, 해당 오류를 일으킬 수 있는 공통 초기 접속 결함을 재현·수정한 근거로 사용한다. 연결 후 상대 사망 감지·설정 불일치 거절은 기존 회귀로 함께 확인했다.

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
| R3 | 별도 `TRACE=io`에서 PID별 `getrusage` 경계 사이 이미지·CSV 파일 I/O 부재 확인(경계 `VmRSS` 읽기 구분) |
| R4/R5 | `run_bench.sh`의 `--no-save` 적용 및 `CLOCK_REALTIME`의 `sem_timedwait` 전용 사용 확인 |
| R6/R7 | 집계 CSV 백분위 열 확인; 응용 내 `R7-OK` 외 백엔드명 출현 0 확인 |

참고: [Linux man-pages](https://man7.org/linux/man-pages/) · [fcntl](https://man7.org/linux/man-pages/man3/fcntl.3p.html) · [sigtimedwait](https://man7.org/linux/man-pages/man3/sigtimedwait.3p.html)
