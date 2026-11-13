# IPC Filter Farm

## 소개

Linux IPC 기초 성능을 측정하고, 이미지 필터 응용에 적합한 IPC를 예측·검증·개선하는 프로젝트다.

필수 비교(`pipe`, `sysv_mq`, `sysv_shm`), 선택 확장(`fifo`, `posix_shm`), 기초 벤치와 측정 도구를 포함한다. **Linux 기초 측정 산출물·빌드·단위·응용 통합·장애·정리 검사는 통과했다. 응용 정식 측정은 아직 실행하지 않았다.** 상세는 [검증 판정](docs/04-test/01-verification.md#현재-검증-판정)과 [범위](docs/01-func/01-requirements.md#범위)를 따른다.

## 기술 스택

C11·POSIX, pthread, Make, Bash를 사용하며 그래프 생성에만 Python 3·matplotlib을 쓴다. 실행·측정은 Linux에서 수행한다([구현 제약](docs/03-tech/03-protocol.md#구현-제약)).

## 시작하기

대상 서버 사양은 [서버 환경 기록](docs/04-test/02-server-environment.md), 디렉터리 배치는 [소스 구조](docs/03-tech/01-architecture.md#소스-구조)를 참고한다.

### 빌드

Ubuntu Linux에서 gcc, Make, Bash, GNU coreutils(`timeout`), util-linux, `strace`, Python 3·matplotlib이 필요하다.

```bash
make
make phase1-pipe phase1-sysv_mq phase1-sysv_shm
make phase2-pipe phase2-sysv_mq phase2-sysv_shm
```

`make`는 전체 바이너리·테스트·측정 도구를 빌드한다. 개별 타깃은 `build/phase1/<IPC>/basic_server,basic_client` 또는 `build/phase2/<IPC>/server,client`와 `pipe_run`을 생성하고 해당 IPC 소스만 링크한다. 새 meta 형식은 구버전과 호환되지 않으므로 업데이트 전 기존 바이너리로 잔존 자원을 정리한다.

## 사용 방법

### 이미지 필터 실행

`--filter`로 이미지 필터를 선택한다([세부 연산](docs/03-tech/03-protocol.md#데이터와-정합성)).

| 옵션 | 효과 |
| --- | --- |
| `gray` | RGB 밝기 변환 흑백 이미지 |
| `blur` | 3×3 평균 블러 |
| `edge` | Sobel 경계 검출 |

익명 `pipe`는 부모가 FD를 상속해야 하므로 `pipe_run`으로 실행한다(`--output`은 새 경로 지정).

```bash
./pipe_run --clients=2 --filter=gray,edge --size=256 --depth=4 \
  --warmup=10 --count=100 --output=pipe-demo
```

`sysv_mq`·`sysv_shm`·`fifo`·`posix_shm`은 서버와 클라이언트를 별도 터미널에서 실행한다.

```bash
./server --ipc=fifo --team=1 --size=256 --clients=1 --count=100
./client --ipc=fifo --team=1 --size=256 --id=1 --filter=gray --count=100 --csv=gray.csv
```

완료 시 입력·결과 이미지 1쌍을 저장한다(`--no-save`로 생략, `--quiet`로 진행률 출력 억제). `--csv`는 측정 seq별 RTT와 `<csv>.cpu.csv`를 저장한다. 워밍업·팀 번호·식별자·저장 조건은 [실행 인터페이스](docs/01-func/01-requirements.md#실행-인터페이스)를 따른다. 같은 팀의 MQ/SHM 테스트는 순차 실행한다.

### 자원 정리

비정상 종료 후 key 경로를 지정해 회수한다(실행 중인 프로세스가 있으면 거절).

```bash
make clean-ipc KEY=/tmp/ipcff.key
./basic_clean_ipc /tmp/ipcff.key
```

### 기초 측정 → 예측 작성 → 응용 측정

```bash
RESULTS=bench/results/basic-run bash bench/run_bench.sh basic
python3 bench/plot.py bench/results/basic-run
RESULTS=bench/results/app-run bash bench/run_bench.sh app
python3 bench/plot.py bench/results/app-run
RESULTS=bench/results/improvement-run bash bench/run_bench.sh improve
python3 bench/plot.py bench/results/improvement-run
```

구현 전 예측 작성·원문 보존은 [원본 과제 조건](docs/assignment.md#필수-수행-순서)을 따른다. `bench/freeze_prediction.sh`와 `PREDICTION`을 통한 보관본 첨부는 선택 사항이다.

기본값은 워밍업 100회·클라당 1,000회·3반복이며, `COUNT/WARMUP/REPEATS/WORKERS/TEAM/BENCH_TIMEOUT/RESULTS` 변경 시 환경 파일에 기록된다. S1의 기본 크기는 [벤치 설계](docs/03-tech/01-architecture.md#2단계-응용-측정)를 따르며 제출 검사는 작은 페이로드 구간의 누락도 거절한다. `BASIC_SIZES/APP_SIZES` 부분 실행은 최종 측정으로 쓰지 않는다. S5는 기본 `msgmnb`를 측정하고, `S5_MSGMNB=65536`과 `sudo` 권한이 있으면 확대 측정 후 원복한다. governor·affinity는 자동 변경하지 않는다([측정과 집계](docs/03-tech/01-architecture.md#측정과-집계)).

`bench/results/`의 원자료·동결본·생성 산출물은 로컬에 보존하며 Git 추적에서 제외한다. 새 체크아웃에서 기존 측정을 이어가려면 별도 보존본을 해당 경로에 복원하고 체크섬을 확인한다. 기초 원자료의 서버 위치는 [기초 측정 결과](docs/basic-measurement.md#측정-조건과-근거)를 따른다.

### 데모·진단·제출 패키지

```bash
bash bench/demo.sh sysv_shm
TRACE=summary bash bench/demo.sh pipe
TRACE=summary bash bench/demo.sh sysv_mq
TRACE=summary bash bench/demo.sh sysv_shm
TRACE=io bash bench/demo.sh sysv_shm
make package-draft
```

데모는 GRAY/EDGE 2클라의 전후 이미지·CSV·통계·종료 후 `ipcs`를 저장한다. `strace` 진단(`TRACE=summary|io`)은 정식 성능 측정과 분리한다([판정 기준](docs/04-test/01-verification.md#검증-항목과-판정-기준)).

소스 미리보기는 `submission/osproject_1-draft.tar`를 생성한다. 정식 패키지는 모든 근거가 준비된 뒤 생성하며, 새 디렉터리에 풀어 Ubuntu+gcc 빌드·테스트를 재확인한다([남은 작업](docs/03-tech/01-architecture.md#남은-작업과-리스크)).

```bash
BASIC_RESULTS=bench/results/basic-run APP_RESULTS=bench/results/app-run \
IMPROVEMENT_RESULTS=bench/results/improvement-run \
FINAL_REPORT=path/to/report.pdf PRESENTATION=path/to/presentation.pptx VIDEO=path/to/presentation.mp4 \
make package
```

## 테스트

루트에서 실행한다(GNU `timeout` 필요, 상세는 [검증 전략](docs/04-test/01-verification.md#검증-전략)).

```bash
make check-headers
make test
make test-mq
make test-shm
make test-app IPC=fifo
DEPTH=1 make test-app IPC=sysv_shm
DEPTH=4 make test-app IPC=sysv_shm
DEPTH=16 make test-app IPC=sysv_shm
make test-pipe
make test-bench
python3 tests/test_check_results.py
```

`IPC`는 `fifo`, `sysv_mq`, `sysv_shm`, `posix_shm`을 지원하며, 개별 loopback은 `./tests/test_ipc_loopback <backend>`로 실행한다. 팀 검사는 1·2팀 자원이 없는 Linux 환경에서 순차 실행한다.

## 관련 문서

- [작업 지침](AGENTS.md): 작업 순서와 문서 관리 규칙.
- [과제 요약](docs/assignment.md): 일정·필수 조건·평가·제출 형식.
- [과제 안내문 원문](docs/assignment-guide.md): 제공 DOCX 변환본.
- [최종보고서 서식 원문](docs/final-report-template.md): 제공 DOCX 목차·안내·빈 표 보존본.
- [보고서 초안](docs/report.md): 서식 순서의 초안과 미측정 항목 구분.
- [기초 측정 결과](docs/basic-measurement.md): 1단계 실측·그래프·무결성 검증과 해석의 한계.
- [IPC 구현·성능 예측](docs/ipc-prediction.md): 구현 전 예측 원문과 구현 후 보완 및 검증 계획.
- [AI 오류 검증 기록](docs/ai-record.md): 확인·정정한 AI 코드 오류와 검증 범위.
- [발표자료 초안](submission/presentation-draft.pptx): 별첨 순서의 PPT 초안.

### 문서 분류 기준

<!-- doc-standard:start — doc-standard에서 동기화한다. 이 구간은 직접 고치지 않는다. -->
| 위치 | 담는 내용 |
| --- | --- |
| [`docs/01-func/`](docs/01-func/01-requirements.md) | 범위, 역할별 기능, 업무 정책, 완료 기준 |
| [`docs/03-tech/`](docs/03-tech/README.md) | 아키텍처, 소스 구조, 데이터 모델, REST/IPC 계약 |
| [`docs/04-test/`](docs/04-test/README.md) | 검증 방법, 판정 기준, 최신 판정과 증거 |
| `docs/` 루트·주제 폴더 | 비명세 자료(과제 안내, 보고서, 발표, 협업 규칙, 자료 수집 기록) |
| 코드 저장소 루트 README | 로컬 설치·실행·테스트 명령 |

API 상세 원본은 OpenAPI·라우터·DTO이며 명세에는 원본 위치와 한계만 적는다. 배포 절차는 `05-ops/`(없으면 기술 명세)에 두고 로컬 실행 명령은 README를 링크한다.
<!-- doc-standard:end -->
