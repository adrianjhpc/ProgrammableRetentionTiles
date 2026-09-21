#!/bin/sh

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 TRACED_STRUCTURES RETENTION_SIM OUTPUT_DIR" >&2
    exit 2
fi

benchmark_executable=$1
simulator_executable=$2
output_directory=$3
mkdir -p "$output_directory"

check_hint_placements() {
    benchmark=$1
    shift
    trace="$output_directory/$benchmark.policy_test.rttrace"
    report="$output_directory/$benchmark.hint.policy_test.txt"

    "$benchmark_executable" "$benchmark" "$trace" >/dev/null
    "$simulator_executable" --trace "$trace" --policy hint >"$report"
    grep -Fqx "status=SAFE" "$report"
    for expected in "$@"; do
        grep -Fqx "$expected" "$report"
    done

    oracle_report="$output_directory/$benchmark.oracle.policy_test.txt"
    "$simulator_executable" --trace "$trace" --policy oracle \
        >"$oracle_report"
    grep -Fqx "status=SAFE" "$oracle_report"

    ephemeral_report="$output_directory/$benchmark.ephemeral.policy_test.txt"
    "$simulator_executable" --trace "$trace" --policy ephemeral \
        >"$ephemeral_report"
    grep -Fqx "status=UNSAFE" "$ephemeral_report"
}

check_hint_placements bfs \
    "structure.bfs_frontiers.placement=EPHEMERAL" \
    "structure.bfs_graph.placement=DURABLE" \
    "structure.bfs_visited.placement=EPOCH"

check_hint_placements hash_join \
    "structure.join_hash_table.placement=EPOCH" \
    "structure.join_inputs.placement=DURABLE" \
    "structure.join_matches.placement=EPHEMERAL"

check_hint_placements stencil \
    "structure.stencil_coefficients.placement=DURABLE" \
    "structure.stencil_grids.placement=EPOCH" \
    "structure.stencil_scratch.placement=EPHEMERAL"

durable_report="$output_directory/stencil.durable.policy_test.txt"
"$simulator_executable" \
    --trace "$output_directory/stencil.policy_test.rttrace" \
    --policy durable >"$durable_report"
grep -Fqx "structure.stencil_coefficients.placement=DURABLE" \
    "$durable_report"
grep -Fqx "structure.stencil_grids.placement=DURABLE" "$durable_report"
grep -Fqx "structure.stencil_scratch.placement=DURABLE" "$durable_report"

echo "benchmark_replay=PASS"
