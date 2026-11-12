#!/usr/bin/env bash
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: check_results.sh basic-dir app-dir improvement-dir' >&2; exit 1; }
basic=$1; app=$2; improvement=$3
for dir in "$basic" "$app" "$improvement"; do
    (cd "$dir" && sha256sum -c SHA256SUMS)
    test -s "$dir/median.csv"
done
grep -q '^phase=basic ' "$basic/environment.txt"
grep -q '^phase=app ' "$app/environment.txt"
grep -q '^phase=improve ' "$improvement/environment.txt"
awk -F, 'NR>1 {
    if($22<1||$23=="smoke") exit 1;
    seen[$1,$3,$4,$23]=1
} END {
    split("pipe sysv_mq sysv_shm",b," ");split("64 1024 16384 262144 1048576",s," ");split("1 2 4 8",c," ");split("latency throughput",m," ");
    for(i=1;i<=3;i++)for(j=1;j<=5;j++)for(k=1;k<=4;k++)for(l=1;l<=2;l++)
        if(!seen[b[i],s[j],c[k],m[l]])exit 1
}' "$basic/basic.csv"
matrix() {
    local file=$1 column=$2 values=$3
    awk -F, -v col="$column" -v values="$values" 'NR>1{if($22<1||$23=="smoke")exit 1;seen[$1,$(col)]=1} END{
        split("pipe sysv_mq sysv_shm",b," ");n=split(values,v," ");
        for(i=1;i<=3;i++)for(j=1;j<=n;j++)if(!seen[b[i],v[j]])exit 1
    }' "$file"
}
matrix "$app/s1.csv" 3 '256 512 1024 2048'
matrix "$app/s2.csv" 4 '1 2 4'
matrix "$app/s3.csv" 5 'gray blur edge'
matrix "$app/s4.csv" 6 '1 4 16'
matrix "$improvement/improvement.csv" 23 'baseline inplace'
awk -F, 'NR>1&&$1=="sysv_mq"&&$3==1024&&$23=="default"{found=1}END{if(!found)exit 1}' "$app/s5.csv"
for graph in basic-payload-latency basic-payload-throughput basic-payload-messages; do test -s "$basic/graphs/$graph.png"; done
for size in 64 1024 16384 262144 1048576; do
    test -s "$basic/graphs/basic-clients-$size-latency.png"
    test -s "$basic/graphs/basic-clients-$size-throughput.png"
done
for graph in app-payload-latency app-payload-throughput app-clients-throughput app-cpu; do test -s "$app/graphs/$graph.png"; done
test -s "$improvement/graphs/improvement.png"
echo '결과 해시·최소 실험 조합·그래프 존재 확인 OK (해석·인과관계 판정은 별도)'
