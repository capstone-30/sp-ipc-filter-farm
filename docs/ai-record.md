# AI 활용·오류 검증 기록 — IPC Filter Farm

[문서 안내](../README.md#관련-문서) · [과제 규정](assignment-guide.md#7-생성형-ai-활용-규정) · [최종보고서 서식 8.2절](final-report-template.md#82-생성형-ai-활용-내용)

이 문서는 [최종보고서 서식 8.2절](final-report-template.md#82-생성형-ai-활용-내용)에 맞춰 생성형 AI 활용 범위, AI 오류 검증 사례, 코드 결함 수정 내용과 부록 첨부 체크리스트를 정리한다. 모든 성능 수치와 검증 결과는 AI 추정이 아니라 Ubuntu Linux 서버의 직접 측정·실행 로그에서 확보했다.

## 1. 활용 단계와 방법

저장소 산출물과 수정 이력에서 확인되는 보조 활용 범위는 아래와 같다. 실제 사용한 AI 서비스·모델명과 팀원별 대표 프롬프트 원문은 최종 제출 전 팀원이 직접 기입한다.

| 단계 | 활용 내용 | 팀원 확인·기입 항목 |
| --- | --- | --- |
| 설계·명세 정리 | 요구사항·프로토콜·공정성 규칙(R1~R7) 문서 구조화 및 체크리스트 정리 | [기입 필요: 사용 AI 도구·대표 프롬프트] |
| 구현·디버깅 | 공용 모듈(`bqueue`, `filter`, `resource`), IPC 백엔드 및 오류·자원 정리 분기 검토 | [기입 필요: 사용 AI 도구·대표 프롬프트] |
| 측정·집계·패키징 | `run_bench.sh`, `aggregate.c`, `plot.py`, `check_results.sh`, `package.sh` 작성 보조 | [기입 필요: 사용 AI 도구·대표 프롬프트] |
| 분석·보고서·발표 | 실측 CSV·`strace`·`ipcs` 원자료 검증 스크립트와 최종보고서·발표자료 초안 정리 | [기입 필요: 사용 AI 도구·대표 프롬프트] |

## 2. AI가 틀렸던 사례 (최소 2건)

| 사례 | AI의 제안·설명 | 실제로 확인한 내용 | 확인 방법과 조치 |
| --- | --- | --- | --- |
| 1 | `src/server.c`의 `respond()`에서 응답 송신 후 `if (rc == 0 && 측정 응답) ... else if (errno != EPIPE) fail(...)` 구조로 작성했다. | `rc == 0`으로 정상 송신한 워밍업 응답(`job.hdr.seq < opt.warmup`)도 첫 번째 `if` 조건을 만족하지 못해 `else if` 분기로 진입하며, 이전에 남아 있던 `errno`에 따라 정상 응답을 실패로 오판할 수 있었다. | 정적 코드 검토로 결함을 확인하고 송신 실패 검사(`if (rc != 0)`)와 측정 응답 카운트(`else if (job.hdr.seq >= opt.warmup && job.hdr.type == MSG_RESP)`)를 분리했다([server.c](../src/server.c#L125-L134)). 수정 후 2026-11-19 Linux 24단계 회귀와 2026-11-19~20 정식 재측정(534회 전건 워밍업 100회 포함, [linux-remeasure-20261119.log](../tests/logs/linux-remeasure-20261119.log))을 통과했다. |
| 2 | `src/common/resource.c`에서 컨텍스트 스위치 계측을 위해 플랫폼 분기 없이 `rusage.ru_nvcsw`·`ru_nivcsw` 필드를 무조건 참조하도록 작성했다. | macOS(Darwin)의 `_POSIX_C_SOURCE=200809L` 조건에서 해당 필드가 노출되지 않아 컴파일 오류 4건이 발생했다. | macOS 컴파일·구문 검사로 오류 4건을 확인하고 `#ifdef __linux__`로 Linux 전용 `resource_save()` 경로를 분리했으며, 비Linux 환경에서는 `ENOTSUP`를 반환하도록 수정했다([resource.c](../src/common/resource.c#L38-L60)). 수정 후 macOS 컴파일 재검사([macos-assignment-contract.log](../tests/logs/macos-assignment-contract.log))와 2026-11-19~20 Linux 회귀·정식 재측정([linux-remeasure-20261119.log](../tests/logs/linux-remeasure-20261119.log))을 통과했다. |

## 3. AI 생성 코드 결함 및 추가 검증 사항

- **오류 처리 분기 결합 결함(사례 1)**: `respond()`에서 송신 성공 여부(`rc != 0`)와 워밍업 이후 측정 응답 집계를 하나의 `if / else if`로 묶어 워밍업 정상 경로가 오류 처리로 흐르는 문제가 있었다. 송신 직후 `int e = errno;`로 오류 번호를 보존하고 `free(job.payload)`와 `release()` 이후 실패 분기와 측정 카운트 분기를 완전히 분리해 해결했다.
- **이식성 가드 누락(사례 2)**: Linux 전용 `getrusage` 확장 필드와 `/proc/self/status` 의존 경로에 조건부 컴파일 가드가 빠져 로컬 macOS 구문·단위 검사가 실패했다. `#ifdef __linux__` 분기와 `ENOTSUP` 반환으로 로컬 도구 검증과 Linux 실측 경로를 안전하게 구분했다.
- **회귀 테스트로 보강한 IPC 경계 방어**: 별도의 코드 점검·회귀 과정에서 메타/FIFO 심볼릭·하드링크·비정규 파일 거절, 초기화 실패 시 생성 자원만 롤백, 빈 `MSG_BYE` 체크섬 검증 누락 보강을 추가로 확인하고 수정했다([macos-risk-fixes.log](../tests/logs/macos-risk-fixes.log), [linux-risk-fixes.log](../tests/logs/linux-risk-fixes.log)). 이는 회귀 검증으로 보강한 품질 개선 사항이며 AI 단독 오류 사례 2건과 구분해 기록한다.

## 4. 부록 첨부 체크리스트 (주요 AI 대화 기록)

[과제 안내문 7절](assignment-guide.md#7-생성형-ai-활용-규정)에 따라 최종보고서 부록에 주요 AI 대화 기록을 첨부해야 한다.

- [ ] 설계·구현·디버깅 단계의 주요 AI 대화 내보내기 파일(`.md` / `.pdf` / `.html`) 또는 화면 캡처 확보
- [ ] 위 1절 표의 사용 AI 도구명과 대표 프롬프트 원문 기입
- [ ] 사례 1·2의 수정 전후 검토 대화 캡처 확보
- [ ] 최종보고서 8.2절 및 9장 부록에 파일/캡처 첨부 확인

전체 코드·측정 검증 판정은 [테스트 명세](04-test/01-verification.md#현재-검증-판정)를 따른다.
