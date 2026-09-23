#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 TRACED_TILED_MATMUL RETENTION_SIM OUTPUT_DIR PROFILE" >&2
    exit 2
fi

benchmark=$1
simulator=$2
output_directory=$3
profile=$4
mkdir -p "$output_directory"

metric() {
    key=$1
    file=$2
    awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

run_case() {
    tile=$1
    trace="$output_directory/tiled_matmul_32_${tile}.reuse_test.rttrace"
    capture="$output_directory/tiled_matmul_32_${tile}.reuse_test.capture.txt"
    report="$output_directory/tiled_matmul_32_${tile}.reuse_test.durable.txt"
    lifetime="$output_directory/tiled_matmul_32_${tile}.reuse_test.lifetime.txt"

    "$benchmark" --trace "$trace" --dimension 32 --tile "$tile" \
        --panel-width 4 >"$capture"
    grep -Fq "benchmark=tiled_matmul" "$capture"
    grep -Fq "status=PASS" "$capture"

    "$simulator" --trace "$trace" --policy durable --profile "$profile" \
        >"$report"
    grep -Fqx "status=SAFE" "$report"

    "$simulator" --analyze-lifetimes "$trace" --profile "$profile" \
        --thresholds 64,128,512,1024,4096,16384,65536 >"$lifetime"
    grep -Fqx "analysis=lifetime status=PASS" "$lifetime"
}

run_case 4
run_case 8

report4="$output_directory/tiled_matmul_32_4.reuse_test.durable.txt"
report8="$output_directory/tiled_matmul_32_8.reuse_test.durable.txt"
lifetime4="$output_directory/tiled_matmul_32_4.reuse_test.lifetime.txt"
lifetime8="$output_directory/tiled_matmul_32_8.reuse_test.lifetime.txt"

# Packing a tile once per K panel reduces durable A/B reads and microtile
# writes in direct proportion to the tile width. Reused microtile reads and
# accumulator traffic are independent of tile width for fixed N and panel.
test "$(metric structure.matmul_inputs.read_bytes "$report4")" = "131072"
test "$(metric structure.matmul_inputs.read_bytes "$report8")" = "65536"
test "$(metric structure.matmul_a_microtile.write_bytes "$report4")" = "65536"
test "$(metric structure.matmul_a_microtile.write_bytes "$report8")" = "32768"
test "$(metric structure.matmul_b_microtile.write_bytes "$report4")" = "65536"
test "$(metric structure.matmul_b_microtile.write_bytes "$report8")" = "32768"
test "$(metric structure.matmul_a_microtile.read_bytes "$report4")" = "262144"
test "$(metric structure.matmul_a_microtile.read_bytes "$report8")" = "262144"
test "$(metric structure.matmul_accumulator_tile.read_bytes "$report4")" = "73728"
test "$(metric structure.matmul_accumulator_tile.read_bytes "$report8")" = "73728"

test "$(metric structure.matmul_a_microtile.allocated_bytes "$report4")" = "128"
test "$(metric structure.matmul_a_microtile.allocated_bytes "$report8")" = "256"

input4=$(metric structure.matmul_inputs.read_bytes "$report4")
input8=$(metric structure.matmul_inputs.read_bytes "$report8")
cycles4=$(metric cycles "$report4")
cycles8=$(metric cycles "$report8")
energy4=$(metric energy_pj "$report4")
energy8=$(metric energy_pj "$report8")
a_lifetime4=$(metric lifetime.matmul_a_microtile.max_cycles "$lifetime4")
a_lifetime8=$(metric lifetime.matmul_a_microtile.max_cycles "$lifetime8")
accumulator_lifetime4=$(metric \
    lifetime.matmul_accumulator_tile.max_cycles "$lifetime4")
accumulator_lifetime8=$(metric \
    lifetime.matmul_accumulator_tile.max_cycles "$lifetime8")

awk -v smaller="$input8" -v larger="$input4" \
    'BEGIN { exit !(smaller * 2 == larger) }'
awk -v smaller="$cycles8" -v larger="$cycles4" \
    'BEGIN { exit !(smaller < larger) }'
awk -v smaller="$energy8" -v larger="$energy4" \
    'BEGIN { exit !(smaller < larger) }'
awk -v short="$a_lifetime4" -v long="$a_lifetime8" \
    'BEGIN { exit !(short < long) }'
awk -v short="$accumulator_lifetime4" -v long="$accumulator_lifetime8" \
    'BEGIN { exit !(short < long) }'

echo "tiled_matmul=PASS input_read_bytes_tile4=$input4 input_read_bytes_tile8=$input8 cycles_tile4=$cycles4 cycles_tile8=$cycles8 energy_pj_tile4=$energy4 energy_pj_tile8=$energy8 a_microtile_lifetime_tile4=$a_lifetime4 a_microtile_lifetime_tile8=$a_lifetime8"
