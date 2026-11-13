#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
[[ $(uname -s) == Linux ]] || { echo 'Linux required' >&2; exit 1; }
backend=${1:-sysv_shm}
case "$backend" in pipe|sysv_mq|sysv_shm) ;; *) echo 'pipe|sysv_mq|sysv_shm required' >&2; exit 1;; esac
trace=${TRACE:-none}; prefix=()
case "$trace" in none|summary|io) ;; *) echo 'TRACE=none|summary|io required' >&2; exit 1;; esac
work=${RESULTS:-"$root/bench/results/demo-$backend-$trace-$(date -u +%Y%m%dT%H%M%SZ)-$$"}
[[ ! -e $work ]] || { echo '새 결과 디렉터리를 지정하세요.' >&2; exit 1; }
mkdir -p "$work"; cd "$work"; work=$PWD
key="$work/key"; pids=()
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    for pid in "${pids[@]}"; do kill -INT "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    "$root/clean_ipc" "$key" >>cleanup.log 2>&1 || status=1
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
date -u '+recorded_at=%FT%TZ' >environment.txt
printf 'diagnostic/demo only; TRACE=%s; not performance evidence\n' "$trace" >>environment.txt
ipcs -q -m -s >ipcs-before.txt
tracer() {
    prefix=()
    if [[ $trace == summary ]]; then prefix=(strace -c -f -o "$work/$1.strace-summary.txt"); fi
    if [[ $trace == io ]]; then prefix=(strace -f -tt -yy -e trace=openat,read,write,pread64,pwrite64,close,getrusage,clone,clone3,fork,vfork,execve -o "$work/$1.io.trace"); fi
}
common=(--team="${TEAM:-1}" --size=256 --depth=4 --warmup=10 --count=100 "--key=$key")
[[ $trace == none ]] || common+=(--quiet)
if [[ $backend == pipe ]]; then
    tracer all
    timeout --kill-after=10s 120s "${prefix[@]}" "$root/pipe_run" --clients=2 --filter=gray,edge --output="$work" "${common[@]}" >launcher.log 2>launcher.err & pids=($!)
else
    tracer server
    timeout --kill-after=10s 120s "${prefix[@]}" "$root/server" "--ipc=$backend" --clients=2 "--cpu-csv=$work/server.cpu.csv" "${common[@]}" >server.log 2>server.err & pids=($!)
    "$root/bench/inspect" "$key" >channel.txt
    for id in 1 2; do
        tracer "client-$id"
        filter=gray; [[ $id == 1 ]] || filter=edge
        timeout --kill-after=10s 120s "${prefix[@]}" "$root/client" "--ipc=$backend" "--id=$id" "--filter=$filter" "--csv=$work/client-$id.csv" "${common[@]}" >"client-$id.log" 2>"client-$id.err" & pids+=($!)
    done
fi
for pid in "${pids[@]}"; do wait "$pid"; done
pids=()
"$root/clean_ipc" "$key" >cleanup.log
ipcs -q -m -s >ipcs-after.txt
"$root/bench/aggregate" "$work" "$backend" demo 256 2 mixed 4 100 10 1 diagnostic 196608 "$work/diagnostic.csv"
printf 'Demo: %s\n' "$work"
