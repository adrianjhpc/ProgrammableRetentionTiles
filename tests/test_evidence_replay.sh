#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 RETENTION_SIM OUTPUT_DIR PROFILE_DIR TRACE_DIR" >&2
    exit 2
fi

simulator=$1
output_directory=$2
profile_directory=$3
trace_directory=$4
mkdir -p "$output_directory"

maintenance_trace="$trace_directory/evidence_maintenance.rttrace"
maintenance_profile="$profile_directory/evidence_maintenance.profile"

hint_report="$output_directory/evidence.hint.txt"
refresh_report="$output_directory/evidence.refresh.txt"
adaptive_report="$output_directory/evidence.adaptive.txt"
json_report="$output_directory/evidence.adaptive.json"

"$simulator" --trace "$maintenance_trace" --policy hint \
    --profile "$maintenance_profile" >"$hint_report"
grep -Fqx "status=UNSAFE" "$hint_report"
grep -Fqx "performance_metrics_valid=false" "$hint_report"
grep -Fqx "failure.expired_accesses=1" "$hint_report"

set +e
"$simulator" --trace "$maintenance_trace" --policy hint \
    --profile "$maintenance_profile" --require-safe >/dev/null
require_safe_status=$?
set -e
[ "$require_safe_status" -eq 3 ]

"$simulator" --trace "$maintenance_trace" --policy refresh \
    --profile "$maintenance_profile" --maintenance-guard 2 \
    >"$refresh_report"
grep -Fqx "status=SAFE" "$refresh_report"
awk -F= '$1 == "refreshes" { exit !($2 > 0) }' "$refresh_report"

"$simulator" --trace "$maintenance_trace" --policy adaptive \
    --profile "$maintenance_profile" --maintenance-guard 2 \
    --json "$json_report" >"$adaptive_report"
grep -Fqx "status=SAFE" "$adaptive_report"
awk -F= '$1 == "migrations" { exit !($2 > 0) }' "$adaptive_report"
grep -Fq '"schema": "RTMEM_EVIDENCE_REPORT_1"' "$json_report"
grep -Fq '"performance_metrics_valid": true' "$json_report"

capacity_report="$output_directory/evidence.capacity.txt"
"$simulator" --trace "$trace_directory/evidence_capacity.rttrace" \
    --policy hint --profile "$profile_directory/evidence_capacity.profile" \
    >"$capacity_report"
grep -Fqx "status=UNSAFE" "$capacity_report"
grep -Fqx "failure.allocation_failures=1" "$capacity_report"
grep -Fqx "failure.rejected_access_events=2" "$capacity_report"
grep -Fqx "performance_metrics_valid=false" "$capacity_report"

untouched_report="$output_directory/evidence.untouched.txt"
"$simulator" \
    --trace "$trace_directory/evidence_no_implicit_write.rttrace" \
    --policy hint --profile "$maintenance_profile" >"$untouched_report"
grep -Fqx "writes=0" "$untouched_report"
grep -Fqx "modeled_dynamic_memory_energy_pj=0.00" "$untouched_report"

one_channel_report="$output_directory/evidence.parallel.1ch.txt"
two_channel_report="$output_directory/evidence.parallel.2ch.txt"
"$simulator" --trace "$trace_directory/evidence_parallel.rttrace" \
    --policy hint \
    --profile "$profile_directory/evidence_parallel_1ch.profile" \
    >"$one_channel_report"
"$simulator" --trace "$trace_directory/evidence_parallel.rttrace" \
    --policy hint \
    --profile "$profile_directory/evidence_parallel_2ch.profile" \
    >"$two_channel_report"
one_cycle=$(awk -F= '$1 == "cycles" { print $2; exit }' \
    "$one_channel_report")
two_cycle=$(awk -F= '$1 == "cycles" { print $2; exit }' \
    "$two_channel_report")
awk -v one="$one_cycle" -v two="$two_cycle" \
    'BEGIN { exit !(two < one) }'

echo "evidence_replay=PASS one_channel_cycles=$one_cycle two_channel_cycles=$two_cycle"
