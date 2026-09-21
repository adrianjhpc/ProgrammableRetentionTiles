# Workload tracing and policy replay

## Purpose

The workload trace separates application behavior from retention-memory
policy. A benchmark is executed once on the host, and the captured memory and
compute events can then be replayed against several placement policies without
rerunning the benchmark.

The replay results are architectural estimates. They are not measurements of
host execution time or FPGA performance.

## Generate and replay the example

```sh
make trace-example
```

This command runs the traced matrix-multiplication kernel and then replays its
output using the hint and oracle policies. The individual commands are:

```sh
./build/traced_matmul build/matmul.rttrace

./build/retention_sim --trace build/matmul.rttrace --policy hint
./build/retention_sim --trace build/matmul.rttrace --policy ephemeral
./build/retention_sim --trace build/matmul.rttrace --policy epoch
./build/retention_sim --trace build/matmul.rttrace --policy durable
./build/retention_sim --trace build/matmul.rttrace --policy oracle
```

For a larger benchmark, select a higher-capacity device profile:

```sh
./build/retention_sim --trace build/workload.rttrace --policy oracle \
  --profile profiles/large_experiment.profile
```

| Policy | Placement behavior |
|---|---|
| `hint` | Starts with the allocation hint and follows later `HINT` events |
| `ephemeral` | Places every buffer in the shortest-retention class |
| `epoch` | Places every buffer in the middle class |
| `durable` | Places every buffer in the strongest class |
| `oracle` | Uses the complete trace to choose a conservative weakest class per buffer |

Oracle placement accounts for the maximum modeled access latency while
estimating data age. It is deliberately conservative and is a comparison bound,
not an implementable online policy.

## C++ capture API

```cpp
rtmem::runtime runtime;
rtmem::trace trace("workload.rttrace");
runtime.attach_trace(trace);

{
    rtmem::policy policy(rtmem::retention_class::ephemeral);
    rtmem::region region(runtime, policy, "kernel");
    rtmem::buffer data(region, elements * sizeof(float), 64);
    rtmem::traced_view<float> values(data, elements);

    runtime.trace_phase("initialize");
    values.store(0, 1.0f);

    runtime.trace_phase("compute");
    const float value = values.load(0);
    runtime.trace_compute(4);
    values.store(1, value * 2.0f);
    runtime.trace_barrier(1);
    values.close();
}

runtime.detach_trace();
trace.flush();
```

`traced_view<T>` requires a trivially copyable type. Its `load` and `store`
methods generate byte-range events, while keeping one mapping open for the
view's lifetime. The stream identifier defaults to zero and can be supplied to
the constructor for multi-stream kernels.

Tracing calls become no-ops when no recorder is attached, so a benchmark can
use the same instrumented accessors in traced and untraced runs.
`trace_compute` records modeled work but does not advance the live runtime's
retention clock.

## C capture API

```c
rt_trace_options trace_options;
rt_trace_options_init(&trace_options, "workload.rttrace");

rt_trace *trace = NULL;
rt_trace_create(&trace_options, &trace);
rt_runtime_attach_trace(runtime, trace);

rt_buffer_write_bytes(buffer, 0, &value, sizeof(value), 0);
rt_runtime_trace_compute(runtime, 0, 4);
rt_buffer_read_bytes(buffer, 0, &result, sizeof(result), 0);

rt_runtime_detach_trace(runtime);
rt_trace_flush(trace);
rt_trace_destroy(trace);
```

For a buffer that is already mapped, call `rt_buffer_trace_access` after each
instrumented application access. The library cannot detect arbitrary loads and
stores through a raw pointer.

## RTTRACE 2 format

The recorder writes a line-oriented, policy-independent format:

```text
RTTRACE 2
ALLOC sequence tick thread buffer bytes alignment hint region
ACCESS sequence tick thread stream R|W buffer offset bytes
COMPUTE sequence tick thread stream cycles
PHASE sequence tick thread name
BARRIER sequence tick thread stream barrier-id
HINT sequence tick thread buffer class
FREE sequence tick thread buffer
```

Sequence numbers establish global event order. Runtime ticks preserve capture
metadata; replay time is advanced by memory operations and explicit `COMPUTE`
events. Names are converted to whitespace-free tokens.

`ALLOC` and `FREE` are emitted automatically. An application reclassification
request is recorded as a hint rather than a mandatory migration, allowing fixed
and oracle policies to ignore it.

## Replay results

Workload-level output includes:

- allocation, free, access, phase, and barrier counts;
- read and write byte totals;
- explicit compute cycles; and
- `unsafe_accesses`, which counts accesses or migrations that could not be
  completed because of expiry or capacity.

The replay also attributes placement and traffic to the allocation's named
region. Fields under `structure.<region>.*` report allocations, allocated
bytes, read bytes, write bytes, and the retention class or classes used by that
structure. Fields under `class.<class>.*` report allocations, allocated bytes,
peak live bytes, and traffic for each modeled class. This makes it possible to
compare mixed per-structure hints with fixed whole-workload policies using the
same trace.

The memory model then reports total cycles, reads, writes, refreshes,
migrations, expirations, capacity failures, and energy in picojoules. A replay
is marked `SAFE` only when `unsafe_accesses` is zero.

## Device profiles

The optional `--profile` argument moves device assumptions out of the trace.
Profiles use this format:

```text
RTMEM_PROFILE 1
# class capacity-lines retention-cycles read-cycles write-cycles read-pJ write-pJ
EPHEMERAL 64 128 2 2 1.0 2.0
EPOCH 64 4096 3 4 1.4 4.5
DURABLE 64 INFINITE 4 12 2.0 12.0
```

Every profile must define all three classes. This lets the same workload trace
be replayed against alternative SST-RAM assumptions without recompiling the
simulator. The supplied values are illustrative and are not measured device
characteristics.

## Modeling boundaries

- Memory is currently modeled at 64-byte line granularity. Multiple byte
  accesses to one line are charged as separate line operations.
- Events are replayed in global sequence order. Thread and stream identifiers
  are retained, but overlapping issue windows are not modeled yet.
- `COMPUTE` cycles must be supplied by the benchmark or another performance
  model; host wall-clock time is not converted automatically.
- The supplied latency, retention, energy, and capacity values are experimental
  profile parameters.
- Raw mapped pointers, conventional allocator pointers, and PMR container
  accesses are not automatically observable. Use traced accessors or compiler
  instrumentation for those workloads.

See `benchmarks.md` for BFS, hash-join, and stencil examples that assign
different retention policies to data structures inside the same kernel.
