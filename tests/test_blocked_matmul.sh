#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 TRACED_BLOCKED_MATMUL RETENTION_SIM OUTPUT_DIR PROFILE" >&2
    exit 2
fi

benchmark_executable=$1
simulator_executable=$2
output_directory=$3
profile=$4
mkdir -p "$output_directory"

trace="$output_directory/blocked_matmul.policy_test.rttrace"
hint_report="$output_directory/blocked_matmul.hint.policy_test.txt"
durable_report="$output_directory/blocked_matmul.durable.policy_test.txt"
oracle_report="$output_directory/blocked_matmul.oracle.policy_test.txt"
ephemeral_report="$output_directory/blocked_matmul.ephemeral.policy_test.txt"
epoch_report="$output_directory/blocked_matmul.epoch.policy_test.txt"

"$benchmark_executable" --trace "$trace" >/dev/null

for policy in hint durable oracle ephemeral epoch; do
    eval report=\$${policy}_report
    "$simulator_executable" --trace "$trace" --policy "$policy" \
        --profile "$profile" >"$report"
done

grep -Fqx "status=SAFE" "$hint_report"
grep -Fqx "structure.matmul_inputs.placement=DURABLE" "$hint_report"
grep -Fqx "structure.matmul_output.placement=DURABLE" "$hint_report"
grep -Fqx "structure.matmul_accumulator_tile.placement=EPOCH" "$hint_report"
grep -Fqx "structure.matmul_a_panel.placement=EPHEMERAL" "$hint_report"
grep -Fqx "structure.matmul_b_panel.placement=EPHEMERAL" "$hint_report"

grep -Fqx "status=SAFE" "$durable_report"
grep -Fqx "status=SAFE" "$oracle_report"
grep -Fqx "status=UNSAFE" "$ephemeral_report"
grep -Fqx "status=UNSAFE" "$epoch_report"

# For this deterministic trace, the conservative oracle should recover the
# same per-structure placement selected by the programmer hints.
for placement in \
    "structure.matmul_inputs.placement=DURABLE" \
    "structure.matmul_output.placement=DURABLE" \
    "structure.matmul_accumulator_tile.placement=EPOCH" \
    "structure.matmul_a_panel.placement=EPHEMERAL" \
    "structure.matmul_b_panel.placement=EPHEMERAL"; do
    grep -Fqx "$placement" "$oracle_report"
done

hint_cycles=$(awk -F= '$1 == "cycles" { print $2; exit }' "$hint_report")
durable_cycles=$(awk -F= '$1 == "cycles" { print $2; exit }' "$durable_report")
hint_energy=$(awk -F= '$1 == "energy_pj" { print $2; exit }' "$hint_report")
durable_energy=$(awk -F= '$1 == "energy_pj" { print $2; exit }' "$durable_report")

awk -v mixed="$hint_cycles" -v durable="$durable_cycles" \
    'BEGIN { exit !(mixed < durable) }'
awk -v mixed="$hint_energy" -v durable="$durable_energy" \
    'BEGIN { exit !(mixed < durable) }'

echo "blocked_matmul=PASS hint_cycles=$hint_cycles durable_cycles=$durable_cycles hint_energy_pj=$hint_energy durable_energy_pj=$durable_energy"
