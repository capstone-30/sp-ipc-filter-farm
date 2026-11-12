# IPC 프로토콜과 구현 제약

로컬 IPC의 메시지·정합성·구현 계약이다(HTTP/REST 미제공). CLI·목표 범위는 [요구사항](../01-func/01-requirements.md), 검증은 [테스트 명세](../04-test/README.md)를 따른다.

## 데이터와 정합성

| 계약 | 단일 원본 헤더 |
| --- | --- |
| enum, `msg_hdr_t`, `IPC_MAGIC/MAX_PAYLOAD/SLOT_COUNT`, FNV-1a 32bit | [proto.h](../../src/common/proto.h) |
| 백엔드 6함수, `size/depth`, 송수신 개별 mutex, `g_stop` | [ipc.h](../../src/ipc/ipc.h) |
| meta 잠금·`ready`·ID 수명 | [ipc_meta.h](../../src/ipc/ipc_meta.h) |
| 유계 큐·작업 소유권 | [bqueue.h](../../src/common/bqueue.h) |
| PPM/합성, 필터, 타이머/통계 | [ppm.h](../../src/common/ppm.h) · [filter.h](../../src/common/filter.h) · [timer.h](../../src/common/timer.h) |
| MQ 청크, `CHUNK_PAYLOAD/MQ_MSGSZ` | [ipc_sysv_mq.h](../../src/ipc/ipc_sysv_mq.h) |
| SHM 링·SEM 인덱스/초기값·정렬 | [ipc_sysv_shm.h](../../src/ipc/ipc_sysv_shm.h) · [ipc_posix_shm.h](../../src/ipc/ipc_posix_shm.h) |

- 메시지 검증: 응용은 `MSG_REQ/MSG_RESP`의 `width/height/channels/filter/type`, 64bit 곱 `payload_len`, 체크섬을 검증한다. 백엔드는 할당·복사 전 `cap/MAX_PAYLOAD`·ID·헤더 길이를 검사한다.
- 라우팅·오류: 링 인덱스는 `client_id-1`이며 출처(FD, MQ 키, SHM 링)와 대조한다. 라우팅 가능한 응용 오류만 `MSG_ERR`(`payload_len=0`)를 반환하고, 프레이밍·출처 불일치는 무응답 퇴역 처리한다([채널 수명](01-architecture.md#채널-수명)). `MSG_BYE`(`payload_len=0`)는 [종료](01-architecture.md#종료와-유한-대기) 전용이다.
- 필터 연산: GRAY=`0.299R+0.587G+0.114B`(3채널 복제), BLUR=3×3 평균, EDGE=Sobel magnitude(0~255 clamp), 경계 픽셀은 clamp 복제한다. 기본은 입출력 분리 버퍼이며 `--inplace-gray`에서만 GRAY in-place를 허용한다.

## 구현 제약

- 리팩터링·확장 시 기존 CLI·프로토콜·필터 결과·측정 경계·자원 소유권·종료 계약과 개별 빌드 링크 규칙을 보존한다.
- 디렉터리 배치([소스 구조](01-architecture.md#소스-구조))와 루트 기준 실행 경로를 유지하며, `.gitignore`는 빌드 산출물·데모 이미지·OS 메타데이터만 제외한다(원자료·검증 로그 보존).
- C11/POSIX(`libc/pthread/rt`)만 사용하며 Python·matplotlib은 `bench/plot.py`에만 허용한다.
- 실행·채점 환경은 [과제 조건](../assignment.md#공통-측정구현-조건)과 [서버 환경 기록](../04-test/02-server-environment.md)을 따른다(Docker 미사용, macOS 검사는 Linux 실행 증거로 불인정).
- `-Wall -Wextra -Werror` 경고 0 및 파일당 줄 상한(백엔드 400, `server/client` 500, 기타 300)을 준수한다.
- 모든 syscall 반환값 검사·`errno` 보존·`-1` 전파를 수행하며, 응용 내부에서 `ipcs/ipcrm/strace`를 직접 호출하지 않는다(`make clean-ipc` 진단 예외).
