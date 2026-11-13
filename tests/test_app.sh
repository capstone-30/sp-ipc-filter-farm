#!/usr/bin/env bash
set -euo pipefail
backend=${1:-fifo}
depth=${DEPTH:-4}
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d /tmp/ipcff-app.XXXXXX)
server_pid= client_pid= second_pid=
cleanup() {
    for pid in "$client_pid" "$second_pid" "$server_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then kill -INT "$pid"; fi
    done
    wait || true
}
trap cleanup EXIT
cd "$work"
key="$work/key"
server() { "$root/server" --ipc="$backend" --key="$key" "$@"; }
client() { "$root/client" --ipc="$backend" --key="$key" "$@"; }
clean_ipc() {
    if compgen -G "$key.*" >/dev/null; then printf '잔존 IPC: %s\n' "$key" >&2; exit 1; fi
}
check_csv() {
    awk -F, -v id="$2" -v count="$3" -v bytes="$4" '
      NR==1 {if ($0 != "client_id,seq,rtt_ns,payload_bytes") exit 1; next}
      {if (NF!=4 || $1!=id || $2!~/^[0-9]+$/ || $2>=count || seen[$2]++ ||
           $3!~/^[0-9]+$/ || $3<=0 || $4!=bytes) bad=1}
      END {if (bad || NR!=count+1) exit 1; for(i=0;i<count;i++) if(seen[i]!=1) exit 1}
    ' "$1"
}

server --size=256 --clients=1 --count=100 > server.log 2>server.err & server_pid=$!
client --size=256 --id=1 --filter=gray --count=100 --csv=gray.csv > gray.log 2>gray.err & client_pid=$!
wait "$client_pid"; client_pid=
wait "$server_pid"; server_pid=
check_csv gray.csv 1 100 196608
"$root/tests/test_images"
grep -q 'matched=100 mismatches=0' gray.log
for name in p50 p95 p99 max mean; do grep -q "$name=" gray.log; done
test ! -s server.err && test ! -s gray.err
clean_ipc
printf 'Application: GRAY 100장·CSV seq 전건·통계·BYE 종료 OK\n'

server --size=64 --clients=1 >server.log 2>server.err & server_pid=$!
"$root/tests/test_app_protocol" "$key" "$backend"
wait "$server_pid"; server_pid=
clean_ipc

server --size=64 --clients=1 >server.log 2>server.err & server_pid=$!
"$root/tests/test_app_protocol" "$key" "$backend" --bad-bye
if wait "$server_pid"; then echo 'corrupt BYE accepted' >&2; exit 1; fi
server_pid=
test -s server.err
clean_ipc

server --size=512 --depth="$depth" --clients=2 --count=1000 >server.log 2>server.err & server_pid=$!
client --size=512 --depth="$depth" --id=1 --filter=gray --count=1000 --no-save --csv=one.csv >one.log 2>one.err & client_pid=$!
client --size=512 --depth="$depth" --id=2 --filter=edge --count=1000 --no-save --csv=two.csv >two.log 2>two.err & second_pid=$!
wait "$client_pid"; client_pid=
wait "$second_pid"; second_pid=
wait "$server_pid"; server_pid=
check_csv one.csv 1 1000 786432
check_csv two.csv 2 1000 786432
test ! -s one.err && test ! -s two.err && test ! -s server.err
test ! -e client-2-output.ppm
clean_ipc
printf 'Application: 2클라 × 1000장·GRAY/EDGE·depth=%s seq 전건 OK\n' "$depth"

server --size=64 --depth=16 --clients=1 --count=32 >server.log 2>server.err & server_pid=$!
client --size=64 --depth=16 --id=1 --filter=blur --count=32 --no-save --csv=blur.csv >blur.log 2>blur.err & client_pid=$!
wait "$client_pid"; client_pid=
wait "$server_pid"; server_pid=
check_csv blur.csv 1 32 12288
clean_ipc

for arg in --count=0 --id=0 --size=2049 --depth=17 --filter=bad --count=4294967296 --warmup=4294967296 --team=0 --team=100; do
    if client --id=1 --filter=gray --count=10 "$arg" >bad.log 2>bad.err; then exit 1; fi
    test "$(wc -l <bad.err)" -eq 1
done
if server --workers=0 >bad.log 2>bad.err; then exit 1; fi
clean_ipc

# background 함수 PID 대신 실제 실행 파일 PID를 얻기 위해 여기서는 직접 실행한다.
"$root/server" --ipc="$backend" --key="$key" >server.log 2>server.err & server_pid=$!
"$root/client" --ipc="$backend" --key="$key" --id=1 --filter=gray --count=1000000 --no-save >long.log 2>long.err & client_pid=$!
for ((i=0;i<200;i++)); do
    if [[ -d /proc/$client_pid/task ]] && [[ $(ls "/proc/$client_pid/task" | wc -l) -eq 4 ]] &&
       [[ $(ls "/proc/$server_pid/task" | wc -l) -eq 5 ]]; then break; fi
    sleep 0.01
done
test "$(ls "/proc/$server_pid/task" | wc -l)" -eq 5
test "$(ls "/proc/$client_pid/task" | wc -l)" -eq 4
kill -INT "$server_pid"
wait "$server_pid"; server_pid=
if wait "$client_pid"; then exit 1; fi
client_pid=
test "$(wc -l <long.err)" -eq 1
clean_ipc
printf 'Application: 서버 5/클라 4 스레드·서버 SIGINT=0·클라 오류 1줄·정리 OK\n'

"$root/server" --ipc="$backend" --key="$key" >server.log 2>server.err & server_pid=$!
"$root/client" --ipc="$backend" --key="$key" --id=1 --filter=gray --count=1000000 --no-save >long.log 2>long.err & client_pid=$!
sleep 0.2
kill -INT "$client_pid"
wait "$client_pid"; client_pid=
kill -INT "$server_pid"
wait "$server_pid"; server_pid=
clean_ipc
printf 'Application: 클라 SIGINT=0·서버 재시작·정리 OK\n'
printf 'Artifacts: %s\n' "$work"
