# Multi-structure retention benchmarks

`traced_structures` contains three small, deterministic kernels. Each kernel
uses several data structures with different retention hints, records one
policy-independent trace, and verifies its result before the trace is replayed.

| Kernel | Data structure | Hint | Reason |
|---|---|---|---|
| BFS | CSR graph | `DURABLE` | Immutable input spans the whole traversal |
| BFS | Visited map | `EPOCH` | Mutable state spans multiple levels |
| BFS | Frontiers | `EPHEMERAL` | Ping-pong queues are consumed at the next level |
| Hash join | Build/probe inputs | `DURABLE` | Inputs span build and probe phases |
| Hash join | Hash table | `EPOCH` | Mutable table spans the join epoch |
| Hash join | Match output | `EPHEMERAL` | Results are checked immediately |
| Stencil | Coefficients | `DURABLE` | Read-only constants span every iteration |
| Stencil | Ping-pong grids | `EPOCH` | Grid values cross iteration boundaries |
| Stencil | Tile scratchpad | `EPHEMERAL` | A tile is overwritten before the next tile |

These are intentionally semantic hints, not claims that every input must use
the strongest physical write. The `oracle` replay shows how a complete-trace
upper bound can make a weaker choice when the modeled lifetime permits it.

## Run the complete comparison

```sh
make benchmarks
```

This builds the benchmark and simulator, creates these traces:

```text
build/bfs.rttrace
build/hash_join.rttrace
build/stencil.rttrace
```

and replays every trace under `hint`, `ephemeral`, `epoch`, `durable`, and
`oracle`. The kernel runs only once to create each trace; changing replay policy
does not rerun the C++ algorithm.

To run one experiment manually:

```sh
./build/traced_structures bfs build/bfs.rttrace
./build/retention_sim --trace build/bfs.rttrace --policy hint
./build/retention_sim --trace build/bfs.rttrace --policy durable
./build/retention_sim --trace build/bfs.rttrace --policy oracle
```

Run the algorithms without recording or simulation with:

```sh
./build/traced_structures bfs
./build/traced_structures hash_join
./build/traced_structures stencil
```

Each command prints a deterministic checksum and `status=PASS` after checking
the actual kernel result.

## Read the results

Replay reports contain both aggregate estimates and structure attribution. For
example, a hinted BFS replay includes:

```text
structure.bfs_frontiers.placement=EPHEMERAL
structure.bfs_graph.placement=DURABLE
structure.bfs_visited.placement=EPOCH
```

For each structure the report also gives allocation, read-byte, and write-byte
totals. `class.EPHEMERAL.*`, `class.EPOCH.*`, and `class.DURABLE.*` fields show
the traffic and peak live bytes assigned to each modeled memory class.

With the supplied default profile, the current deterministic results are:

| Kernel | Policy | Safety | Cycles | Energy (pJ) |
|---|---|---:|---:|---:|
| BFS | `hint` | Safe | 534 | 433.70 |
| BFS | `ephemeral` | Unsafe | 206 | 141.00 |
| BFS | `epoch` | Safe | 368 | 288.80 |
| BFS | `durable` | Safe | 804 | 680.00 |
| BFS | `oracle` | Safe | 288 | 210.90 |
| Hash join | `hint` | Safe | 787 | 639.20 |
| Hash join | `ephemeral` | Unsafe | 316 | 208.00 |
| Hash join | `epoch` | Safe | 581 | 455.50 |
| Hash join | `durable` | Safe | 1,294 | 1,102.00 |
| Hash join | `oracle` | Safe | 555 | 429.80 |
| Stencil | `hint` | Safe | 1,792 | 1,046.80 |
| Stencil | `ephemeral` | Unsafe | 860 | 467.00 |
| Stencil | `epoch` | Safe | 1,906 | 1,170.50 |
| Stencil | `durable` | Safe | 3,436 | 2,480.00 |
| Stencil | `oracle` | Safe | 1,906 | 1,170.50 |

An unsafe result is not a valid performance point: its low cycle or energy
number includes reads that missed because data expired. Compare costs only
among runs with `status=SAFE`.

The values above are model output, not host timings or measured SST-RAM/FPGA
performance. Change `profiles/default.profile` or pass another profile to test
device assumptions without changing or rerunning the benchmark:

```sh
./build/retention_sim --trace build/stencil.rttrace --policy hint \
  --profile profiles/large_experiment.profile
```

## Add another kernel

Use one named `rtmem::region` for each structure/policy combination, access its
buffers through `rtmem::traced_view<T>`, and add modeled work with
`runtime.trace_compute`. Phase and barrier events make algorithm boundaries
visible in the trace. The region name becomes the `structure.<name>.*` prefix
in replay output.

Keep correctness checking inside the benchmark. The replay engine models the
recorded events; it does not execute or validate the original algorithm.
