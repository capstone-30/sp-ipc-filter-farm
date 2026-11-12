#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d /tmp/ipcff-clean.XXXXXX)
pid= client= shm_path=
trap 'for p in "$client" "$pid"; do if [[ -n "$p" ]]; then kill -INT "$p" 2>/dev/null || true; fi; done; wait || true' EXIT
start() {
    "$root/server" --ipc="$backend" --key="$key" --size=64 >"$work/server.log" 2>"$work/server.err" & pid=$!
    # ready는 stdout 버퍼링에 의존하지 않고 클라의 한 번 정상 왕복으로 확인한다.
    "$root/client" --ipc="$backend" --key="$key" --size=64 --id=4 --filter=gray --count=1 --no-save >"$work/client.log" 2>"$work/client.err"
    shm_path=
    if [[ "$backend" = posix_shm ]]; then
        shm_path=/dev/shm/osproject_1_shm
        test -f "$shm_path"
    fi
}
empty() { if compgen -G "$key*" >/dev/null; then echo "leftover: $key" >&2; exit 1; fi; if [[ -n "$shm_path" ]]; then test ! -e "$shm_path"; fi; }
for backend in fifo sysv_mq sysv_shm posix_shm; do
    key="$work/$backend"
    start
    if "$root/clean_ipc" "$key" >"$work/clean.log" 2>&1; then exit 1; fi
    kill -KILL "$pid"; wait "$pid" 2>/dev/null || true; pid=
    "$root/clean_ipc" "$key"
    empty
    "$root/clean_ipc" "$key"
    start
    kill -KILL "$pid"; wait "$pid" 2>/dev/null || true; pid=
    start
    kill -INT "$pid"; wait "$pid"; pid=
    empty
    printf 'Cleanup: %s active refusal, SIGKILL cleanup, repeated cleanup, restart OK\n' "$backend"
done
# 다른 백엔드의 알 수 없는 기록이 있으면 정상 기록도 보존한다.
backend=sysv_shm; key="$work/mixed"; start
kill -KILL "$pid"; wait "$pid" 2>/dev/null || true; pid=
printf 'unknown\n' > "$key.mq"
if "$root/clean_ipc" "$key" >"$work/clean.log" 2>&1; then exit 1; fi
test -f "$key.shm" && test -f "$key.meta"
test "$(cat "$key.mq")" = unknown
rm "$key.mq"
"$root/clean_ipc" "$key"; empty
printf 'Cleanup: mixed unknown record preserves known resources OK\n'
for backend in sysv_shm posix_shm; do
key="$work/isolation-$backend"
start
"$root/client" --ipc="$backend" --key="$key" --size=64 --id=1 --filter=gray --count=1000000 --no-save >"$work/live.log" 2>"$work/live.err" & client=$!
sleep .2
kill -KILL "$client"; wait "$client" 2>/dev/null || true; client=
"$root/client" --ipc="$backend" --key="$key" --size=64 --id=2 --filter=edge --count=1000 --no-save >"$work/survivor.log" 2>"$work/survivor.err"
grep -q 'matched=1000 mismatches=0' "$work/survivor.log"
kill -INT "$pid"
# 자식 wait 대신 생존 여부를 최대 5초 확인한다.
for ((i=0;i<500;i++)); do if ! kill -0 "$pid" 2>/dev/null; then break; fi; sleep .01; done
if kill -0 "$pid" 2>/dev/null; then echo 'shutdown exceeded 5 seconds' >&2; exit 1; fi
wait "$pid"; pid=; empty
printf '%s application: SIGKILL client isolation, 1000 matches, shutdown <=5 seconds OK\n' "$backend"
done
# FIFO 경로를 일반 파일로 교체하면 정리를 거절해야 한다.
backend=fifo; key="$work/preserve"; start
kill -KILL "$pid"; wait "$pid" 2>/dev/null || true; pid=
rm "$key.req.1"
printf 'keep\n' > "$key.req.1"
if "$root/clean_ipc" "$key" >"$work/clean.log" 2>&1; then exit 1; fi
test "$(cat "$key.req.1")" = keep
rm "$key.req.1"
"$root/clean_ipc" "$key"; empty
printf 'Cleanup: unrelated regular file preserved OK\nArtifacts: %s\n' "$work"
