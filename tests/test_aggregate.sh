#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
aggregate=${AGGREGATE:-$root/bench/aggregate}
work=$(mktemp -d)
# 합성 단위 테스트 fixture다. 측정 원자료로 보존하지 않는다.
cat >"$work/server.cpu.csv" <<'EOF'
ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,first_send_ns,last_recv_ns
1,2,3,4,100,120,0,0
EOF
cat >"$work/client-1.csv.cpu.csv" <<'EOF'
ru_utime_ms,ru_stime_ms,nvcsw,nivcsw,rss_start_kib,rss_end_kib,first_send_ns,last_recv_ns
2,3,4,5,200,220,100,1000000100
EOF
cat >"$work/client-1.csv" <<'EOF'
client_id,seq,rtt_ns,payload_bytes
1,3,4000,64
1,2,1000,64
EOF
"$aggregate" --header >"$work/result.csv"
"$aggregate" "$work" pipe basic 64 1 gray 1 2 2 1 latency 64 "$work/result.csv"
awk -F, 'NR==2 {if($8!=1 || $10!=4 || $12!=2.5 || $13!=0.000128 || $14!=2 || $15!=3 || $16!=5 || $17!=7 || $18!=9 || $19!=300 || $20!=340 || $25!=2) exit 1; found=1} END{if(!found) exit 1}' "$work/result.csv"
printf '1,3,4000,64\n' >>"$work/client-1.csv"
if "$aggregate" "$work" pipe basic 64 1 gray 1 2 2 1 latency 64 "$work/result.csv" 2>"$work/expected.err"; then exit 1; fi
test "$(wc -l <"$work/result.csv")" -eq 2
echo 'aggregate: nearest-rank·전체 시간 경계·자원 합계·중복 seq 거절 OK'
echo "fixture: $work"
