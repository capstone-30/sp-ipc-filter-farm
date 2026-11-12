#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d /tmp/ipcff-pipe-test.XXXXXX)
cd "$work"
launcher=
cleanup() {
    if [[ -n $launcher ]]; then kill -INT "$launcher" 2>/dev/null || true; wait "$launcher" 2>/dev/null || true; fi
}
trap cleanup EXIT
[[ $(uname -s) == Linux ]] || { echo 'Linux required' >&2; exit 1; }
for mode in app basic; do
    args=()
    if [[ $mode == basic ]]; then args+=(--basic --bytes=64); fi
    for warmup in default zero; do
        extra=()
        if [[ $warmup == zero ]]; then extra+=(--warmup=0); fi
        dir="$work/$mode-$warmup"
        "$root/pipe_run" "${args[@]}" --clients=1 --size=32 --count=10 --quiet --no-save "${extra[@]}" --output="$dir"
        for name in server client-1; do
            test -s "$dir/$name.log"
            test -f "$dir/$name.err" && test ! -s "$dir/$name.err"
        done
        test "$(wc -l <"$dir/client-1.csv")" -eq 11
        test -s "$dir/client-1.csv.cpu.csv"
        test ! -e "$dir/server.cpu.csv"
    done
done
for depth in 1 4 16; do
    "$root/pipe_run" --clients=2 --filter=gray,edge --size=32 --depth="$depth" --count=20 --warmup=5 --quiet --output="$work/d$depth"
    "$root/bench/aggregate" "$work/d$depth" pipe test 32 2 mixed "$depth" 20 5 1 check 3072 "$work/summary.csv"
done
cp client-1-output.ppm baseline.ppm
"$root/pipe_run" --clients=1 --filter=gray --size=32 --depth=16 --count=20 --warmup=5 --quiet --inplace-gray --output="$work/inplace"
cmp baseline.ppm client-1-output.ppm
"$root/pipe_run" --clients=2 --filter=gray,edge --size=512 --depth=4 --count=1000 --warmup=10 --quiet --no-save --output="$work/integration"
"$root/bench/aggregate" "$work/integration" pipe integration 512 2 mixed 4 1000 10 1 check 786432 "$work/summary.csv"
"$root/pipe_run" --basic --clients=8 --bytes=1048576 --depth=4 --count=4 --warmup=2 --quiet --output="$work/basic"
"$root/bench/aggregate" "$work/basic" pipe basic 1048576 8 gray 4 4 2 1 check 1048576 "$work/summary.csv"
if "$root/client" --ipc=pipe --id=1 --filter=gray --count=1 >bad.log 2>bad.err; then exit 1; fi
# 프로세스 사망 시 부모가 나머지 자식에 종료를 전달하고 모두 회수해야 한다.
"$root/pipe_run" --clients=2 --filter=gray,edge --size=64 --count=100000 --warmup=2 --quiet --no-save --key="$work/fault-key" --output="$work/fault" >fault.log 2>fault.err & launcher=$!
"$root/bench/inspect" "$work/fault-key" >channel.txt
for ((i=0;i<500;i++)); do
    children=$(cat "/proc/$launcher/task/$launcher/children")
    [[ $(wc -w <<<"$children") -eq 3 ]] && break
    sleep 0.01
done
read -r server_pid rest <<<"$children"
test -n "$server_pid"
kill -KILL "$server_pid"
if wait "$launcher"; then exit 1; fi
launcher=
for pid in $server_pid $rest; do test ! -e "/proc/$pid"; done
"$root/clean_ipc" "$work/fault-key"
echo "pipe: 부분 전송(1MiB)·8클라·다중 필터·개선 동등성·상대 사망/회수 OK; $work"
