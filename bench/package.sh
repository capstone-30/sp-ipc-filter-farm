#!/usr/bin/env bash
set -euo pipefail
export COPYFILE_DISABLE=1
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
team=${TEAM:-1}; draft=${1:-}
[[ $team =~ ^[1-9][0-9]?$ && ( -z $draft || $draft == --draft ) ]] || { echo 'TEAM=1..99; optional --draft' >&2; exit 1; }
suffix=
if [[ $draft == --draft ]]; then suffix=-draft
else
    for var in BASIC_RESULTS APP_RESULTS IMPROVEMENT_RESULTS FINAL_REPORT PRESENTATION VIDEO; do
        [[ -n ${!var:-} ]] || { echo "$var 경로가 필요합니다. 소스 미리보기는 make package-draft." >&2; exit 1; }
    done
    for path in "$BASIC_RESULTS/basic.csv" "$APP_RESULTS/s1.csv" "$APP_RESULTS/s2.csv" "$APP_RESULTS/s3.csv" \
        "$APP_RESULTS/s4.csv" "$APP_RESULTS/s5.csv" "$IMPROVEMENT_RESULTS/improvement.csv" \
        "$FINAL_REPORT" "$PRESENTATION" "$VIDEO"; do
        [[ -s $path ]] || { echo "필수 제출 근거 없음: $path" >&2; exit 1; }
    done
    for dir in "$BASIC_RESULTS" "$APP_RESULTS" "$IMPROVEMENT_RESULTS"; do
        [[ -s $dir/median.csv && -d $dir/graphs ]] || { echo "집계·그래프 없음: $dir" >&2; exit 1; }
    done
    bash bench/check_results.sh "$BASIC_RESULTS" "$APP_RESULTS" "$IMPROVEMENT_RESULTS"
    echo '결과 검사와 파일 존재 확인을 수행합니다. 내용·과거 예측 인정·발표 완료 여부는 최종 검토가 필요합니다.' >&2
fi
mkdir -p submission
archive="$root/submission/osproject_${team}${suffix}.tar"
[[ ! -e $archive ]] || { echo "이미 존재합니다: $archive" >&2; exit 1; }
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
files=(Makefile README.md AGENTS.md .github src tests bench docs)
tar --exclude='bench/results' --exclude='*.o' --exclude='__pycache__' --exclude='bench/aggregate' --exclude='bench/inspect' --exclude='tests/test_proto' \
    --exclude='tests/test_bqueue' --exclude='tests/test_ppm' --exclude='tests/test_filter' --exclude='tests/test_timer' \
    --exclude='tests/test_basic_options' --exclude='tests/test_ipc_util' --exclude='tests/test_app_options' --exclude='tests/test_images' --exclude='tests/test_ipc_loopback' \
    --exclude='tests/test_fifo_faults' --exclude='tests/test_app_protocol' --exclude='tests/test_client_protocol' \
    --exclude='tests/test_mq' --exclude='tests/test_shm' --exclude='tests/test_posix' --exclude='tests/test_team' --exclude='tests/test_pipe_protocol' \
    -cf - "${files[@]}" | tar -xf - -C "$stage"
{
    printf 'IPC Filter Farm — generated from README.md + docs/04-test/02-server-environment.md\n'
    [[ -z $suffix ]] || printf 'DRAFT: Linux verification, measurements and submission evidence may be incomplete.\n'
    cat README.md docs/04-test/02-server-environment.md
} >"$stage/readme.txt"
if [[ -f submission/presentation-draft.pptx ]]; then
    mkdir -p "$stage/submission"
    cp submission/presentation-draft.pptx "$stage/submission/"
fi
if [[ -z $suffix ]]; then
    mkdir -p "$stage/evidence" "$stage/submission"
    cp "$FINAL_REPORT" "$stage/submission/report.${FINAL_REPORT##*.}"
    cp "$PRESENTATION" "$stage/submission/presentation.${PRESENTATION##*.}"
    cp "$VIDEO" "$stage/submission/video.${VIDEO##*.}"
    cp -R "$BASIC_RESULTS" "$stage/evidence/basic"
    cp -R "$APP_RESULTS" "$stage/evidence/app"
    cp -R "$IMPROVEMENT_RESULTS" "$stage/evidence/improvement"
    if [[ -n ${PREDICTION:-} ]]; then cp -R "$PREDICTION" "$stage/evidence/prediction"; fi
fi
packed=("${files[@]}" readme.txt)
[[ ! -d $stage/submission ]] || packed+=(submission)
[[ ! -d $stage/evidence ]] || packed+=(evidence)
tar -cf "$stage/package.tar" -C "$stage" "${packed[@]}"
mv -n "$stage/package.tar" "$archive"
[[ ! -e $stage/package.tar ]] || { echo "이미 존재합니다: $archive" >&2; exit 1; }
printf '%s\n' "$archive"
