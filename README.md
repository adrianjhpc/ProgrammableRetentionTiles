# Retention Tiles FPGA Prototype

This project is the first, hardware-free milestone for a tiled memory system
with programmer-visible retention classes. It provides:

- a versioned C ABI for applications and future FPGA backends;
- C++ RAII wrappers and a `std::pmr::memory_resource` adapter;
- regions, mapped buffers, promotion, refresh, expiry and statistics;
- a tested C++ cycle-level reference model;
- deterministic expiry and optional stochastic retention failures;
- refresh, migration, invalidation, capacity and energy accounting;
- a small trace language for replaying workload memory lifetimes;
- a vendor-neutral SystemVerilog metadata controller; and
- synthesis entry points for AMD/Xilinx Vivado and Intel Quartus.

The FPGA model does **not** claim to reproduce MTJ device physics. It is meant
to determine whether retention-aware placement saves enough latency, energy and
bandwidth to justify a later custom multi-retention MRAM test chip.

## Quick start

Only a C++17 compiler and `make` are required for the reference model.

```sh
make test
make examples
./build/retention_sim --trace traces/smoke.trace
```

The public interfaces are:

```text
include/rtmem/rtmem.h     Stable C ABI
include/rtmem/rtmem.hpp   C++17 RAII and std::pmr wrappers
```

The current host backend is deterministic and requires no FPGA. It exercises
the same region and buffer lifecycle that an XRT or Avalon backend will expose.
See `docs/software_api.md` for lifetime, mapping and persistence semantics.

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

These are experimental parameters, not device predictions. Change them in
`sim/retention_model.cpp` after measured or modelled device data is available.

## Trace language

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

See `docs/architecture.md` for the design boundary and
`docs/hardware_targets.md` for the board-specific plan.
