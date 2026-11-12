#!/usr/bin/env bash
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: freeze_prediction.sh docs/ipc-prediction.md basic-results-dir output-dir' >&2; exit 1; }
source=$1; results=$2; output=$3
[[ -s $source && -s $results/basic.csv && ! -e $output ]] || { echo '예측 원문·기초 실측 또는 새 출력 경로를 확인하세요.' >&2; exit 1; }
grep -q '^phase=basic ' "$results/environment.txt" || { echo '기초 정식 측정이 아닙니다.' >&2; exit 1; }
(cd "$results" && sha256sum -c SHA256SUMS)
mkdir -p "$output"
cp "$source" "$output/ipc-prediction.md"
cp "$results/basic.csv" "$output/basic.csv"
cp "$results/environment.txt" "$output/environment.txt"
cp "$results/source.sha256" "$output/basic-source.sha256"
cp "$results/SHA256SUMS" "$output/basic-run.SHA256SUMS"
{
    date -u '+frozen_at=%FT%TZ'
    printf '기존 응용 구현 이후에 동결한 예측입니다. 최초 구현 전 예측 요건의 충족을 주장하지 않습니다.\n'
    printf '기초 원자료 경로: %s\n' "$(cd "$results" && pwd)"
} >"$output/provenance.txt"
(cd "$output" && sha256sum ipc-prediction.md basic.csv environment.txt basic-source.sha256 basic-run.SHA256SUMS provenance.txt >SHA256SUMS)
echo "$output"
