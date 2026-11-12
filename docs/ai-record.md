# AI 활용·오류 검증 기록

[문서 안내](../README.md#관련-문서)

| 사례 | AI 생성 코드·해석 | 확인·정정 | 검증 범위 |
| --- | --- | --- | --- |
| 1 | `responder`에서 `if (rc==0 && 측정 응답) ... else if (errno!=EPIPE) fail`로 작성 | 워밍업 정상 응답도 `else if`로 빠져 이전 `errno`로 실패할 수 있음을 정적 검토로 확인 후, 송신 실패 검사와 측정 카운트 처리를 분리 | 정적 검토 (Linux 통합 검사 미실행) |
| 2 | `resource.c`가 macOS에서 숨겨진 `rusage.ru_nvcsw/ru_nivcsw`를 무조건 참조 | macOS 구문 검사에서 컴파일 오류 4건을 확인하고 Linux 전용 경로 분리 및 타 OS `ENOTSUP` 반환으로 수정 | macOS 컴파일 재검사 통과 (Linux 실측 미실행) |

전체 판정과 남은 작업은 [테스트 명세](04-test/01-verification.md#현재-검증-판정)를 따른다.
