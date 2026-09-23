#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 RETENTION_SIM BLOCKED_MATMUL OUTPUT_DIR PROFILE" >&2
    exit 2
fi

simulator=$1
benchmark=$2
output_directory=$3
profile=$4
mkdir -p "$output_directory"

trace="$output_directory/lifetime_blocked_matmul.rttrace"
report="$output_directory/lifetime_blocked_matmul.txt"
csv="$output_directory/lifetime_blocked_matmul.csv"
json="$output_directory/lifetime_blocked_matmul.json"

"$benchmark" --trace "$trace" --dimension 8 --tile 4 >/dev/null
"$simulator" --analyze-lifetimes "$trace" --profile "$profile" \
    --thresholds 16,128,4096,65536 \
    --lifetime-csv "$csv" --lifetime-json "$json" >"$report"

grep -Fqx "analysis=lifetime status=PASS" "$report"
grep -q '^lifetime.matmul_a_panel.line_versions=' "$report"
grep -q '^lifetime.matmul_accumulator_tile.p95_cycles=' "$report"
grep -q '^ALL,' "$csv"
grep -q '"schema": "RTMEM_LIFETIME_ANALYSIS_1"' "$json"

echo "lifetime_analysis=PASS"
