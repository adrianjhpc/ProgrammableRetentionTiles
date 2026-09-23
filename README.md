# Retention Tiles FPGA Prototype

This project is a software-first prototype for a tiled memory system with
programmer-visible retention classes. It provides:

- a versioned C ABI for applications and future FPGA backends;
- C++ RAII wrappers and a `std::pmr::memory_resource` adapter;
- regions, mapped buffers, promotion, refresh, expiry and statistics;
- a tested C++ cycle-level reference model;
- deterministic expiry and optional stochastic retention failures;
- refresh, migration, invalidation, capacity and energy accounting;
- a small trace language for replaying workload memory lifetimes;
- a vendor-neutral SystemVerilog metadata controller;
- synthesis entry points for AMD/Xilinx Vivado and Intel Quartus;
- an optional native-XRT allocation and migration backend;
- a U280 HLS migration kernel with three independent HBM ports;
- a policy-independent workload tracing API and replay engine; and
- event-driven multi-stream replay, explicit safety failures, maintenance
  policies, profile provenance, energy breakdowns, and JSON evidence reports;
- traced naive, short-panel blocked, and reuse-aware tiled matrix
  multiplication plus BFS, hash-join, and stencil benchmarks for
  per-data-structure policy comparisons.
- U280 migration calibration and blocked-matmul measurement programs; and
- per-structure lifetime analysis plus reproducible retention/capacity sweeps.

The FPGA model does **not** claim to reproduce MTJ device physics. It is meant
to determine whether retention-aware placement saves enough latency, energy and
bandwidth to justify a later custom multi-retention MRAM test chip.

## Quick start

Only a C++17 compiler and `make` are required for the reference model.

```sh
make test
make examples
make trace-example
make blocked-matmul
make tiled-matmul
make tiled-experiments
make benchmarks
./build/retention_sim --trace traces/smoke.trace
```

For proposal-oriented reproducibility guidance and the exact boundary between
modeled evidence and measured claims, see `docs/proposal_evidence.md`.
For the measured and modeled experiment workflow, see `docs/experiments.md`.

The public interfaces are:

```text
include/rtmem/rtmem.h     Stable C ABI
include/rtmem/rtmem.hpp   C++17 RAII and std::pmr wrappers
```

The default host backend is deterministic and requires no FPGA. It exercises
the same region and buffer lifecycle as the optional XRT backend.
See `docs/software_api.md` for lifetime, mapping and persistence semantics.

## Trace a real kernel

`traced_matmul` executes an actual C++ matrix multiplication while recording
allocations, byte-range accesses, compute intervals, phases, barriers, hints,
and frees:

```sh
./build/traced_matmul build/matmul.rttrace
./build/retention_sim --trace build/matmul.rttrace --policy hint
./build/retention_sim --trace build/matmul.rttrace --policy oracle
./build/retention_sim --trace build/matmul.rttrace --policy durable
./build/retention_sim --trace build/matmul.rttrace --policy oracle \
  --profile profiles/large_experiment.profile
```

The same workload can therefore be replayed without rerunning the benchmark.
The `hint` policy follows application retention hints; fixed policies place all
buffers in one class; `oracle` uses future trace knowledge to choose the weakest
conservative class for each buffer. `refresh` and `adaptive` add explicit
maintenance. Use `--require-safe` in automated experiments and `--json FILE`
for machine-readable reports. See `docs/tracing.md` for the API and trace
format.

### Retention-aware blocked matrix multiplication

The blocked kernel keeps the complete A, B, and C matrices durable, uses an
epoch accumulator for one output tile, and repeatedly overwrites ephemeral A
and B panels immediately before each partial dot product:

```sh
./build/traced_blocked_matmul \
  --trace build/blocked_matmul.rttrace \
  --dimension 16 --tile 4
./build/retention_sim --trace build/blocked_matmul.rttrace \
  --policy hint --profile profiles/large_experiment.profile
./build/retention_sim --trace build/blocked_matmul.rttrace \
  --policy durable --profile profiles/large_experiment.profile
```

Under the supplied illustrative profile, mixed placement is safe and uses
fewer modeled cycles and less energy than all-durable placement. All-epoch is
still unsafe because the complete matrices remain kernel-lived. See
`docs/blocked_matmul.md` for the dataflow, commands, results, and interpretation.

### Reuse-aware tiled matrix multiplication

`traced_tiled_matmul` packs an entire `tile x panel_width` A microtile and
`panel_width x tile` B microtile once per K panel, then reuses them across the
output tile. Increasing the tile width reduces durable input reads and
microtile writes while increasing ephemeral capacity and required lifetime:

```sh
./build/traced_tiled_matmul \
  --trace build/tiled_matmul.rttrace \
  --dimension 32 --tile 4 --panel-width 4
./build/retention_sim \
  --analyze-lifetimes build/tiled_matmul.rttrace \
  --profile profiles/proposal_equal_resources.profile \
  --thresholds 64,128,512,1024,2048,4096,8192,16384,65536
```

This benchmark is intended for tile-size, capacity, and retention sweeps. See
`docs/tiled_matmul.md` for the traffic formulas, replay commands, expected
regression results, and interpretation cautions.

## Compare policies within one kernel

`traced_structures_variable` gives separate retention hints to the graph,
mutable state, and scratch/output structures inside each BFS, hash-join, or
stencil kernel. `traced_structures` is retained as the repository's
all-durable BFS comparison. One execution creates a policy-independent trace,
then the simulator replays that exact trace under mixed hints, fixed classes,
and an oracle policy:

```sh
./build/traced_structures_variable bfs build/bfs.rttrace
./build/retention_sim --trace build/bfs.rttrace --policy hint
./build/retention_sim --trace build/bfs.rttrace --policy epoch
./build/retention_sim --trace build/bfs.rttrace --policy durable
./build/retention_sim --trace build/bfs.rttrace --policy oracle
```

Replay output includes `structure.<name>.placement`, read/write bytes per
structure, and traffic/peak-live-byte totals per retention class. Run all three
kernels against all five policies with `make benchmarks`. See
`docs/benchmarks.md` for the policy map, expected default-profile results, and
interpretation rules.

## U280 vertical slice

The first board experiment maps `EPHEMERAL`, `EPOCH`, and `DURABLE` to U280
`HBM[0]`, `HBM[1]`, and `HBM[2]`. Promotion allocates in the destination bank
and runs a 512-bit copy kernel. HBM emulates distinct physical tiles; it does
not emulate MRAM retention physics by itself.

After sourcing your Vitis and XRT setup scripts:

```sh
make -C fpga/u280 \
  PLATFORM=/path/to/xilinx_u280_gen3x16_xdma_1_202211_1.xpfm \
  TARGET=hw
make RTMEM_ENABLE_XRT=1 xrt-example
./build/xrt_vertical_slice \
  fpga/u280/build/rtmem_u280.hw.xclbin 0
```

The last argument may be an XRT device index or a PCIe BDF. Platform names vary
with the installed release; use the `.xpfm` available on the machine. See
`docs/xrt_u280.md` for the API, build variables, and expected output.

Build the complete U280 measurement suite with:

```sh
make RTMEM_ENABLE_XRT=1 xrt-experiments
```

This adds `xrt_migration_benchmark` and `xrt_blocked_matmul`. The simulator can
also report per-structure write-to-last-read distributions with
`--analyze-lifetimes`; `scripts/run_retention_sweep.py` runs retention,
capacity, policy, and seed sweeps while preserving every generated profile and
report.

### C allocator adapter

```c
rt_runtime *runtime = NULL;
rt_runtime_create(NULL, &runtime);

rt_policy policy;
rt_policy_init(&policy, RT_CLASS_EPHEMERAL);

rt_region *iteration = NULL;
rt_region_create(runtime, &policy, "iteration", &iteration);

void *frontier = NULL;
rt_region_malloc(iteration, frontier_bytes, 64, &frontier);
/* Use frontier while the iteration is live. */
rt_region_free_pointer(iteration, frontier);
```

### C++ PMR adapter

```cpp
rtmem::runtime runtime;
rtmem::policy policy{rtmem::retention_class::ephemeral};
rtmem::region iteration{runtime, policy};
rtmem::retention_resource memory{iteration};

std::pmr::vector<Node> frontier{&memory};
```

Use explicit `rt_buffer`/`rtmem::buffer` objects when an application needs a
device address, mapping boundaries, per-buffer promotion, or persistence
ordering.

The default banks are deliberately small and use short cycle counts so that a
test completes quickly:

| Class | Capacity | Nominal retention | Read | Write |
|---|---:|---:|---:|---:|
| `EPHEMERAL` | 64 lines | 128 cycles | 2 cycles | 2 cycles |
| `EPOCH` | 64 lines | 4,096 cycles | 3 cycles | 4 cycles |
| `DURABLE` | 64 lines | effectively infinite | 4 cycles | 12 cycles |

These are experimental parameters, not device predictions. Pass
`--profile profiles/default.profile` explicitly, copy that file, and edit its
capacity, retention, latency, and energy fields when measured or modeled device
data becomes available. `profiles/large_experiment.profile` keeps the same
costs but expands capacity for larger traces.

Use `profiles/proposal_equal_resources.profile` for policy comparisons that
must keep class capacity and channel count constant. The older
`proposal_evidence.profile` intentionally provisions two channels for the
shorter-retention classes and therefore represents a different architecture,
not a retention-only comparison.

`RTMEM_PROFILE 2` additionally identifies the profile and its source, and can
model controller/metadata/ECC overhead, static power, migration setup, and
multiple bank channels. `profiles/proposal_evidence.profile` demonstrates the
format and is explicitly marked as illustrative rather than measured.

## Legacy trace language

Addresses are byte addresses and are converted to 64-byte logical lines.

```text
WRITE <address> <EPHEMERAL|EPOCH|DURABLE> <64-bit-value>
READ <address> EXPECT <64-bit-value>
READ <address> MISS
WAIT <cycles>
REFRESH <address>
MIGRATE <address> <EPHEMERAL|EPOCH|DURABLE>
INVALIDATE <address>
STATS
```

Numbers may be decimal or use the `0x` hexadecimal prefix. Comments start with
`#`.

Use `--mode stochastic --seed N` to replace deterministic expiry with a simple
exponential failure process. Deterministic mode is the preferred mode while
debugging policies.

The recorder produces the newer `RTTRACE 2` workload format. The simulator
auto-detects both formats.

## RTL

`rtl/retention_metadata_controller.sv` implements the first synthesizable
policy core. It records a validity bit, retention class and last-write epoch for
each logical line. It detects expired lines and performs metadata changes for
writes, refreshes, migrations and invalidations. The actual data copy associated
with migration remains the responsibility of the surrounding DMA engine.

Run a synthesis-only Vivado check with:

```sh
vivado -mode batch -source fpga/vivado_synth.tcl \
  -tclargs <exact-fpga-part>
```

For Quartus, set `FPGA_PART` to the exact Stratix 10 device and run:

```sh
FPGA_PART=<exact-device> quartus_sh -t fpga/quartus_synth.tcl
```

No board part is hard-coded because U250/U280 card revisions and Stratix 10
boards can expose different exact device identifiers.

## Recommended board order

1. Run the U280 three-HBM-bank migration vertical slice.
2. Put retention metadata and automatic promotion scheduling on the FPGA.
3. Replay traces at high concurrency with hardware traffic generators.
4. Port the same interface to U250 DDR and Stratix 10 Avalon-MM.

See `docs/architecture.md` for the design boundary and
`docs/hardware_targets.md` for the board-specific plan.
