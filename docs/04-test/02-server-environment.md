# 서버 환경 기록 — IPC Filter Farm

[문서 안내](../../README.md#관련-문서)

제공받은 서버 조회 결과다(조회 시점 미확인). 실행·측정 조건은 [과제 요약](../assignment.md#공통-측정구현-조건)을 따른다.

## 제공 환경

| 항목 | 환경 정보 |
| --- | --- |
| 아키텍처 / OS / 커널 | `x86_64` / Ubuntu 24.04.5 LTS / `6.8.0-147-generic` (`systemd-detect-virt`: `none`) |
| CPU | Intel Xeon Bronze 3106 @ 1.70GHz, 2소켓 × 8코어 × 1스레드(총 16코어, 800~1700MHz, governor `powersave`) |
| NUMA / 셸 범위 | 2노드(`node0=0-3,8-11`, `node1=4-7,12-15`), 허용 CPU `0-15`·메모리 `0-1` (affinity 미설정) |
| 메모리 / Swap | 총 62GiB(available 53GiB) / Swap 2.0GiB(사용 889MiB) |
| 컴파일러 / 도구 | `gcc 13.3.0`; `/usr/bin/` 내 `make`, `timeout`, `strace`, `taskset`, `cpupower`, `python3` 존재 |
| SysV MQ 제한 | `msgmax=8192` B, `msgmnb=16384` B, `msgmni=32000` |
| SysV SHM / SEM 제한 | `shmmax=shmall=18446744073692774399`, `shmmni=4096` / `kernel.sem=32000 1024000000 500 32000` |
| POSIX SHM 공간 | `/dev/shm` 총 32G (사용 0, 가용 32G) |
| 조회 시 부하 | load average `1.07/1.05/1.08`, `vmstat` CPU idle 93%, swap in/out 0 |

## 측정 전 확인

실제 벤치 시점의 환경은 [측정과 집계](../03-tech/01-architecture.md#측정과-집계) 절차에 따라 `environment.txt`에 별도 기록한다.
