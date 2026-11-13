#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
phase=${1:-basic}
[[ $(uname -s) == Linux ]] || { echo 'Linux에서 실행하세요. Docker는 자동 실행하지 않습니다.' >&2; exit 1; }
case "$phase" in basic|app|improve|smoke|s5) ;; *) echo 'usage: run_bench.sh basic|app|improve|smoke|s5' >&2; exit 1;; esac
count=${COUNT:-1000}; warmup=${WARMUP:-100}; repeats=${REPEATS:-3}; team=${TEAM:-1}
limit=${BENCH_TIMEOUT:-86400}; workers=${WORKERS:-2}
for value in "$count" "$warmup" "$repeats" "$team" "$limit" "$workers"; do
    [[ $value =~ ^[1-9][0-9]*$ ]] || { echo '양의 정수 옵션이 필요합니다.' >&2; exit 1; }
done
if [[ $phase == smoke ]]; then count=8; warmup=4; repeats=1; fi
out=${RESULTS:-"bench/results/$(date -u +%Y%m%dT%H%M%SZ)-$phase-$$"}
[[ ! -e $out ]] || { echo "결과 디렉터리가 이미 있습니다: $out" >&2; exit 1; }
mkdir -p "$out/raw"
out=$(cd "$out" && pwd)
pids=(); active_key=; cleaner=./clean_ipc; original_msgmnb=
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    for pid in "${pids[@]}"; do kill -INT "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    if [[ -n $active_key ]]; then "$cleaner" "$active_key" >>"$out/cleanup.log" 2>&1 || status=1; fi
    if [[ -n $original_msgmnb ]]; then
        if ! sudo -n sysctl -w "kernel.msgmnb=$original_msgmnb" >>"$out/environment.txt" 2>&1; then
            echo "복원 실패: sudo sysctl -w kernel.msgmnb=$original_msgmnb 필요" >&2; status=1
        fi
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
{
    date -u '+recorded_at=%FT%TZ'
    printf 'phase=%s count=%s warmup=%s repeats=%s workers=%s team=%s\n' "$phase" "$count" "$warmup" "$repeats" "$workers" "$team"
    printf 'SMOKE=%s (smoke 자료는 과제 성능 결과로 사용하지 않음)\n' "$([[ $phase == smoke ]] && echo yes || echo no)"
    uname -a; cat /etc/os-release; lscpu; free -k; "${CC:-cc}" --version
    systemd-detect-virt || [[ $? == 1 ]]
    taskset -pc $$
    for governor in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [[ ! -f $governor ]] || { printf '%s=' "$governor"; cat "$governor"; }
    done
    sysctl kernel.msgmax kernel.msgmnb kernel.msgmni kernel.shmmax kernel.shmall kernel.sem
    df -k /dev/shm; ipcs -q -m -s
    printf 'wait: FIFO/pipe poll<=100ms; SysV retry=1ms; workers fixed; affinity inherited\n'
    printf 'CPU: RSS start -> getrusage start; getrusage end -> RSS end. RSS is boundary sample, sums may double-count shared pages.\n'
    printf 'basic overhead: each direction payload B + 48B control; MQ adds 20B/chunk plus sizeof(long) mtype (msgsz excludes mtype).\n'
    printf 'MB/s counts one-way payload, not header or response bytes. CPU sums server+clients; individual sidecars retained.\n'
} >"$out/environment.txt" 2>&1
tar --exclude='bench/results' --exclude='__pycache__' --exclude='bench/aggregate' --exclude='bench/inspect' \
    -cf "$out/source.tar" Makefile src tests bench docs/01-func/01-requirements.md docs/03-tech/01-architecture.md README.md
sha256sum "$out/source.tar" >"$out/source.sha256"
if [[ -n ${PREDICTION:-} ]]; then cp -R "$PREDICTION" "$out/prediction"; fi

run_case() {
    local backend=$1 scenario=$2 size=$3 clients=$4 filter=$5 depth=$6 repeat=$7 mode=$8 extra=${9:-}
    [[ $size =~ ^[1-9][0-9]{0,6}$ ]] || { echo '잘못된 크기' >&2; return 1; }
    local basic=0 payload=$((3 * size * size))
    [[ $scenario != basic ]] || { basic=1; payload=$size; }
    if (( basic )); then ((size >= 64 && size <= 1048576)) || return 1
    else ((size <= 2048)) || return 1; fi
    local name="$scenario-$backend-$size-$clients-$filter-$depth-$repeat-$mode"
    local dir="$out/raw/$name" summary="$out/$scenario.csv" prefix=
    mkdir "$dir"
    active_key="$dir/key"
    cleaner=./clean_ipc
    if (( basic )); then prefix=basic_; cleaner=./basic_clean_ipc; fi
    local common=("--team=$team" "--depth=$depth" "--count=$count" "--warmup=$warmup" --quiet "--key=$active_key")
    if (( basic )); then common+=("--bytes=$size"); else common+=("--size=$size"); fi
    local server=("./${prefix}server" "--ipc=$backend" "${common[@]}" "--clients=$clients" "--workers=$workers" "--cpu-csv=$dir/server.cpu.csv")
    [[ -z $extra ]] || server+=("$extra")
    printf '%q ' "${server[@]}" >"$dir/command.txt"; printf '\n' >>"$dir/command.txt"
    if [[ $backend == pipe ]]; then
        local launcher=(./pipe_run "--clients=$clients" "--output=$dir" "--filter=$filter" "--workers=$workers" --no-save "${common[@]}")
        if (( basic )); then launcher+=(--basic); fi
        [[ -z $extra ]] || launcher+=("$extra")
        printf '%q ' "${launcher[@]}" >>"$dir/command.txt"; printf '\n' >>"$dir/command.txt"
        timeout --kill-after=10s "${limit}s" "${launcher[@]}" >"$dir/launcher.log" 2>"$dir/launcher.err" & pids=($!)
        wait "${pids[0]}"; pids=()
    else
        timeout --kill-after=10s "${limit}s" "${server[@]}" >"$dir/server.log" 2>"$dir/server.err" & pids=($!)
        ./bench/inspect "$active_key" >"$dir/channel.txt"
        for ((id=1;id<=clients;id++)); do
            local client=("./${prefix}client" "--ipc=$backend" "${common[@]}" "--id=$id" "--filter=$filter" --no-save "--csv=$dir/client-$id.csv")
            printf '%q ' "${client[@]}" >>"$dir/command.txt"; printf '\n' >>"$dir/command.txt"
            timeout --kill-after=10s "${limit}s" "${client[@]}" >"$dir/client-$id.log" 2>"$dir/client-$id.err" & pids+=($!)
        done
        for pid in "${pids[@]}"; do wait "$pid"; done
        pids=()
    fi
    for err in "$dir"/*.err; do [[ ! -s $err ]] || { cat "$err" >&2; return 1; }; done
    [[ -f $summary ]] || ./bench/aggregate --header >"$summary"
    ./bench/aggregate "$dir" "$backend" "$scenario" "$size" "$clients" "$filter" "$depth" "$count" "$warmup" "$repeat" "$mode" "$payload" "$summary"
    "$cleaner" "$active_key" >"$dir/cleanup.log"
    active_key=
    ipcs -q -m -s >"$dir/ipcs-after.txt"
    printf '완료 %s\n' "$name"
}

for ((repeat=1;repeat<=repeats;repeat++)); do
    for backend in pipe sysv_mq sysv_shm; do
        case "$phase" in
        basic)
            for size in ${BASIC_SIZES:-64 1024 16384 262144 1048576}; do
                for clients in 1 2 4 8; do
                    run_case "$backend" basic "$size" "$clients" gray 1 "$repeat" latency
                    run_case "$backend" basic "$size" "$clients" gray 4 "$repeat" throughput
                done
            done;;
        app)
            for size in ${APP_SIZES:-64 80 96 104 256 512 1024 2048}; do run_case "$backend" s1 "$size" 1 gray 1 "$repeat" app; done
            for clients in 1 2 4; do run_case "$backend" s2 512 "$clients" gray 1 "$repeat" app; done
            for filter in gray blur edge; do run_case "$backend" s3 512 2 "$filter" 1 "$repeat" app; done
            for depth in 1 4 16; do run_case "$backend" s4 512 1 gray "$depth" "$repeat" app; done;;
        improve)
            run_case "$backend" improvement 512 2 gray 4 "$repeat" baseline
            run_case "$backend" improvement 512 2 gray 4 "$repeat" inplace --inplace-gray;;
        smoke)
            run_case "$backend" basic 64 8 gray 1 "$repeat" smoke
            run_case "$backend" s1 32 2 gray 4 "$repeat" smoke
            run_case "$backend" improvement 32 2 gray 4 "$repeat" inplace --inplace-gray;;
        esac
    done
    if [[ $phase == app || $phase == s5 ]]; then
        run_case sysv_mq s5 1024 1 gray 1 "$repeat" default
        if [[ -n ${S5_MSGMNB:-} ]]; then
            [[ $S5_MSGMNB =~ ^[1-9][0-9]*$ ]] || { echo 'S5_MSGMNB must be positive integer' >&2; exit 1; }
            original_msgmnb=$(sysctl -n kernel.msgmnb)
            if sudo -n sysctl -w "kernel.msgmnb=$S5_MSGMNB" >>"$out/environment.txt" 2>&1; then
                run_case sysv_mq s5 1024 1 gray 1 "$repeat" enlarged
                sudo -n sysctl -w "kernel.msgmnb=$original_msgmnb" >>"$out/environment.txt" 2>&1
                original_msgmnb=
            else
                echo 'S5 enlargement unavailable; only default measured.' >>"$out/environment.txt"
                original_msgmnb=
            fi
        else echo 'S5_MSGMNB unset; only default measured.' >>"$out/environment.txt"; fi
    fi
done
(cd "$out" && {
    find raw -type f -print0 | sort -z | xargs -0 sha256sum
    sha256sum ./*.csv environment.txt source.tar
} >SHA256SUMS)
printf '결과: %s\n' "$out"
