#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[[ $(uname -s) == Linux ]] || { echo 'Linux required' >&2; exit 1; }
bash tests/test_aggregate.sh
work=$(mktemp -d /tmp/ipcff-bench-test.XXXXXX)
RESULTS="$work/smoke" bash bench/run_bench.sh smoke
for summary in basic s1 improvement; do
    test "$(wc -l <"$work/smoke/$summary.csv")" -eq 4
done
# 원자료는 워밍업 0..3을 포함하지 않고 4..11의 전 seq를 포함해야 한다.
for csv in "$work/smoke/raw/"*/client-1.csv; do
    awk -F, 'NR>1{if($2<4||$2>11||seen[$2]++)exit 1;n++}END{if(n!=8)exit 1}' "$csv"
done
if ./client --ipc=sysv_shm --id=1 --filter=gray --count=4294967295 --warmup=1 >"$work/invalid.log" 2>&1; then exit 1; fi
echo "bench: 3IPC·기초 8클라·워밍업 제외·자원 sidecar·개선 옵션 OK; $work"
