# Software interface

## Compatibility boundary

`include/rtmem/rtmem.h` is the stable, versioned C ABI. FPGA-specific backends
must implement its runtime, region and buffer operations without changing
application code. `include/rtmem/rtmem.hpp` is a C++17 convenience layer over
the same ABI.

The checked-in backend is `RT_BACKEND_HOST_EMULATED`. XRT and Avalon enum values
reserve their ABI identities but intentionally return `RT_ERROR_UNSUPPORTED`
until their transports are implemented.

## Object lifetime

1. A runtime owns one backend and its retention-class configuration.
2. A region owns a policy and groups allocations with a common semantic life.
3. A buffer belongs to exactly one region.
4. Buffers must be freed before their region is destroyed in C code.
5. C++ buffers keep their region and runtime alive automatically.

`rt_region_end` invalidates every active buffer in the region. The handles may
still be passed to `rt_buffer_free`, but mapping or lifecycle operations fail.

For C programs that want conventional allocator syntax,
`rt_region_malloc`/`rt_region_free_pointer` provide host-coherent pointers with
the owning region's policy. Buffer handles remain the portable interface for
discrete accelerators.

## Retention policies

Each region specifies:

- an initial retention class;
- whether the request is required, preferred or approximate;
- what maintenance action occurs near the deadline; and
- the promotion target when promotion is selected.

The runtime must be polled before deadlines. `poll_guard_ticks` determines how
early a refresh, invalidation or promotion is attempted. A deadline that has
already passed returns `RT_ERROR_EXPIRED`; it is never silently repaired.

If preferred or approximate promotion cannot fit in the destination class, the
host backend falls back to refresh. Required promotion reports a capacity error.

## Mapping and writes

Only one mapping per buffer is permitted by the host backend. Unmapping a
write mapping resets the retention age. Accelerator backends should reset age
when a DMA or kernel write completes; `rt_buffer_mark_written` exists for that
notification.

The host emulator returns stable host addresses, so a pointer remains physically
dereferenceable after unmapping. Applications must nevertheless obey map/unmap
semantics because a discrete FPGA backend may not provide coherent host access.

`std::pmr::memory_resource` is therefore most useful for host-coherent or future
integrated implementations. Its emulator cannot observe arbitrary CPU stores;
region lifetime remains the authoritative semantic boundary.

## Persistence

Retention and crash consistency are separate. `rt_buffer_flush` and
`rt_runtime_fence` establish an ordering point for future persistent backends.
The host emulator implements them with a sequentially consistent CPU fence; it
does not provide filesystem persistence.
