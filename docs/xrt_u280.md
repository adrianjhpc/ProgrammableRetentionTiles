# U280 XRT vertical slice

## What this proves

This checkpoint verifies that one application buffer can move through three
physical memory groups without changing its logical handle:

| Retention class | Kernel port | U280 connection |
|---|---|---|
| `EPHEMERAL` | `bank0` | `HBM[0]` |
| `EPOCH` | `bank1` | `HBM[1]` |
| `DURABLE` | `bank2` | `HBM[2]` |

The host runtime allocates XRT buffer objects in the group associated with the
kernel argument. `rt_buffer_promote` allocates a destination object, invokes
`rtmem_migrate`, waits for completion, and then replaces the buffer's physical
allocation. The example verifies the contents after both promotions.

Migration destinations are created without the normal zero-fill/upload step
because the copy kernel overwrites every padded word. Backend instrumentation
exposes BO allocation, synchronization, submission, and wait time through
`rt_runtime_get_backend_stats`.

This is a topology and software-semantics experiment. U280 HBM does not have
the variable-retention or write-pulse behavior of STT/SST RAM.

## Prerequisites

- a U280 with a compatible installed platform and firmware;
- XRT and Vitis `v++` from a mutually compatible release; and
- the corresponding U280 `.xpfm` path.

Source the Vitis and XRT environment scripts supplied by the installation
before building.

## Build the xclbin

```sh
make -C fpga/u280 \
  PLATFORM=/path/to/installed/u280/platform.xpfm \
  TARGET=hw
```

`TARGET` may also be `sw_emu` or `hw_emu` if the installed platform supports
that flow. The result is
`fpga/u280/build/rtmem_u280.<target>.xclbin`.

The relevant sources are:

- `fpga/u280/rtmem_migrate.cpp`: one pipelined 512-bit word copy per loop;
- `fpga/u280/rtmem_blocked_matmul.cpp`: durable matrices, ephemeral packed
  panels, and an epoch accumulator tile;
- `fpga/u280/connectivity.cfg`: port-to-HBM bindings; and
- `fpga/u280/Makefile`: Vitis compile and link commands.

## Build and run the host application

```sh
make RTMEM_ENABLE_XRT=1 XRT_ROOT=/opt/xilinx/xrt xrt-example
./build/xrt_vertical_slice \
  fpga/u280/build/rtmem_u280.hw.xclbin 0
```

Set `XRT_LIB_DIR` if `libxrt_coreutil` is not in `$XRT_ROOT/lib`. Instead of
device index `0`, the example accepts a BDF such as `0000:65:00.1`.

Successful output shows a different device address for each class followed by
verification of 1 MiB after two migrations. Exact addresses vary by run.

Build the complete measurement programs with `make RTMEM_ENABLE_XRT=1
xrt-experiments`. `xrt_migration_benchmark` records raw and summarized
migration timings; `xrt_blocked_matmul` exercises the three-class tiled
dataflow. See `experiments.md` for commands and interpretation rules.

## Application API

```cpp
rtmem::runtime_options runtime_options;
rtmem::xrt_options xrt;
xrt.xclbin_path = "rtmem_u280.hw.xclbin";
xrt.device_index = 0;

rtmem::runtime runtime(runtime_options, xrt);
rtmem::policy policy(rtmem::retention_class::ephemeral);
rtmem::region region(runtime, policy);
rtmem::buffer data(region, bytes, 64);

void* mapped = data.map(RT_MAP_WRITE);
// Fill mapped data.
data.unmap();
data.promote(rtmem::retention_class::epoch);
```

Explicit buffer objects are required for XRT. The conventional C allocator and
C++ PMR adapter intentionally remain host-coherent-only because an application
must not retain a CPU pointer while the underlying XRT buffer object migrates.

## Current limitations

- Policy evaluation, deadlines, and statistics are host-side and serialized by
  the runtime mutex.
- A promotion blocks until the copy kernel completes.
- Each class has one HBM pseudo-channel and there is one copy compute unit.
- The kernel copies a final padded 64-byte word for non-multiple sizes; the
  runtime exposes only the requested logical byte count.
- Retention latency, energy, and failure behavior are still supplied by the
  simulator, not the HBM design.
