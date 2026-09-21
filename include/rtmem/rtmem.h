#ifndef RTMEM_RTMEM_H
#define RTMEM_RTMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTMEM_API_VERSION 0x00010000u
#define RTMEM_INFINITE_TICKS UINT64_MAX

typedef struct rt_runtime rt_runtime;
typedef struct rt_region rt_region;
typedef struct rt_buffer rt_buffer;
typedef struct rt_trace rt_trace;

typedef enum rt_status {
    RT_OK = 0,
    RT_ERROR_INVALID_ARGUMENT = 1,
    RT_ERROR_NO_MEMORY = 2,
    RT_ERROR_CAPACITY = 3,
    RT_ERROR_EXPIRED = 4,
    RT_ERROR_INVALID_STATE = 5,
    RT_ERROR_UNSUPPORTED = 6,
    RT_ERROR_BUSY = 7,
    RT_ERROR_BACKEND = 8
} rt_status;

typedef enum rt_backend {
    RT_BACKEND_HOST_EMULATED = 0,
    RT_BACKEND_XRT = 1,
    RT_BACKEND_AVALON = 2
} rt_backend;

typedef enum rt_clock_mode {
    RT_CLOCK_MANUAL = 0,
    RT_CLOCK_MONOTONIC = 1
} rt_clock_mode;

typedef enum rt_retention_class {
    RT_CLASS_EPHEMERAL = 0,
    RT_CLASS_EPOCH = 1,
    RT_CLASS_DURABLE = 2,
    RT_CLASS_COUNT = 3
} rt_retention_class;

typedef enum rt_guarantee {
    RT_GUARANTEE_REQUIRED = 0,
    RT_GUARANTEE_PREFERRED = 1,
    RT_GUARANTEE_APPROXIMATE = 2
} rt_guarantee;

typedef enum rt_expiry_action {
    RT_EXPIRY_ERROR = 0,
    RT_EXPIRY_INVALIDATE = 1,
    RT_EXPIRY_REFRESH = 2,
    RT_EXPIRY_PROMOTE = 3
} rt_expiry_action;

typedef enum rt_buffer_state {
    RT_BUFFER_ACTIVE = 0,
    RT_BUFFER_INVALID = 1,
    RT_BUFFER_EXPIRED = 2
} rt_buffer_state;

typedef enum rt_map_flags {
    RT_MAP_READ = 1u << 0,
    RT_MAP_WRITE = 1u << 1
} rt_map_flags;

typedef enum rt_trace_access_kind {
    RT_TRACE_ACCESS_READ = 0,
    RT_TRACE_ACCESS_WRITE = 1
} rt_trace_access_kind;

typedef enum rt_trace_flags {
    RT_TRACE_DEFAULT = 0,
    RT_TRACE_FLUSH_EACH_EVENT = 1u << 0
} rt_trace_flags;

typedef struct rt_runtime_options {
    uint32_t struct_size;
    uint32_t api_version;
    rt_backend backend;
    rt_clock_mode clock_mode;
    uint64_t tick_ns;
    uint64_t poll_guard_ticks;
    uint64_t class_retention_ticks[RT_CLASS_COUNT];
    uint64_t class_capacity_bytes[RT_CLASS_COUNT];
    uint32_t flags;
    uint32_t reserved;
} rt_runtime_options;

/* XRT-specific creation parameters. Strings are copied during creation. */
typedef struct rt_xrt_options {
    uint32_t struct_size;
    uint32_t device_index;
    const char* device_bdf;
    const char* xclbin_path;
    const char* migration_kernel_name;
    uint32_t flags;
    uint32_t reserved;
} rt_xrt_options;

typedef struct rt_trace_options {
    uint32_t struct_size;
    const char* path;
    uint32_t flags;
    uint32_t reserved;
} rt_trace_options;

typedef struct rt_policy {
    uint32_t struct_size;
    rt_retention_class retention_class;
    rt_guarantee guarantee;
    rt_expiry_action on_expiry;
    rt_retention_class promotion_target;
    uint32_t flags;
    double maximum_bit_error_rate;
} rt_policy;

typedef struct rt_runtime_stats {
    uint32_t struct_size;
    uint64_t allocations;
    uint64_t active_buffers;
    uint64_t active_bytes[RT_CLASS_COUNT];
    uint64_t peak_active_bytes;
    uint64_t map_calls;
    uint64_t refreshes;
    uint64_t promotions;
    uint64_t demotions;
    uint64_t invalidations;
    uint64_t expiry_events;
    uint64_t poll_calls;
    uint64_t bytes_migrated;
    uint64_t allocation_failures;
} rt_runtime_stats;

typedef struct rt_region_stats {
    uint32_t struct_size;
    uint64_t allocations;
    uint64_t active_buffers;
    uint64_t active_bytes;
    uint64_t peak_active_bytes;
} rt_region_stats;

typedef struct rt_buffer_info {
    uint32_t struct_size;
    uint64_t size_bytes;
    uint64_t alignment;
    rt_retention_class retention_class;
    rt_guarantee guarantee;
    rt_buffer_state state;
    uint32_t map_count;
    uint64_t created_tick;
    uint64_t last_write_tick;
    uint64_t deadline_tick;
    uint64_t generation;
    uint64_t device_address;
} rt_buffer_info;

void rt_runtime_options_init(rt_runtime_options* options);
void rt_xrt_options_init(rt_xrt_options* options);
void rt_trace_options_init(rt_trace_options* options, const char* path);
void rt_policy_init(rt_policy* policy, rt_retention_class retention_class);
const char* rt_status_string(rt_status status);

rt_status rt_trace_create(const rt_trace_options* options,
                          rt_trace** output_trace);
void rt_trace_destroy(rt_trace* trace);
const char* rt_trace_last_error(const rt_trace* trace);
rt_status rt_trace_flush(rt_trace* trace);

rt_status rt_runtime_create(const rt_runtime_options* options,
                            rt_runtime** output_runtime);
rt_status rt_runtime_create_xrt(const rt_runtime_options* options,
                                const rt_xrt_options* xrt_options,
                                rt_runtime** output_runtime);
void rt_runtime_destroy(rt_runtime* runtime);
const char* rt_runtime_last_error(const rt_runtime* runtime);
rt_status rt_runtime_is_host_coherent(rt_runtime* runtime,
                                      uint32_t* output_supported);
rt_status rt_runtime_attach_trace(rt_runtime* runtime, rt_trace* trace);
rt_status rt_runtime_detach_trace(rt_runtime* runtime);
/* Trace annotations do not advance the live runtime clock. */
rt_status rt_runtime_trace_phase(rt_runtime* runtime, const char* name);
rt_status rt_runtime_trace_compute(rt_runtime* runtime,
                                   uint32_t stream_id,
                                   uint64_t cycles);
rt_status rt_runtime_trace_barrier(rt_runtime* runtime,
                                   uint32_t stream_id,
                                   uint64_t barrier_id);
rt_status rt_runtime_now(rt_runtime* runtime, uint64_t* output_tick);
rt_status rt_runtime_advance(rt_runtime* runtime, uint64_t ticks);
rt_status rt_runtime_poll(rt_runtime* runtime);
rt_status rt_runtime_fence(rt_runtime* runtime);
rt_status rt_runtime_get_stats(rt_runtime* runtime,
                               rt_runtime_stats* output_stats);

rt_status rt_region_create(rt_runtime* runtime,
                           const rt_policy* policy,
                           const char* debug_name,
                           rt_region** output_region);
rt_status rt_region_end(rt_region* region);
void rt_region_destroy(rt_region* region);
rt_status rt_region_get_stats(rt_region* region,
                              rt_region_stats* output_stats);
rt_status rt_region_refresh(rt_region* region);
rt_status rt_region_reclassify(rt_region* region,
                               rt_retention_class destination_class);

/* Host-coherent allocator adapter. The pointer uses the region's policy. */
rt_status rt_region_malloc(rt_region* region,
                           size_t size_bytes,
                           size_t alignment,
                           void** output_pointer);
rt_status rt_region_free_pointer(rt_region* region, void* pointer);

rt_status rt_alloc(rt_region* region,
                   size_t size_bytes,
                   size_t alignment,
                   rt_buffer** output_buffer);
void rt_buffer_free(rt_buffer* buffer);

rt_status rt_buffer_map(rt_buffer* buffer,
                        uint32_t flags,
                        void** output_pointer);
rt_status rt_buffer_unmap(rt_buffer* buffer);
rt_status rt_buffer_mark_written(rt_buffer* buffer);
rt_status rt_buffer_refresh(rt_buffer* buffer);
rt_status rt_buffer_reclassify(rt_buffer* buffer,
                               rt_retention_class destination_class);
rt_status rt_buffer_promote(rt_buffer* buffer,
                            rt_retention_class destination_class);
rt_status rt_buffer_invalidate(rt_buffer* buffer);
rt_status rt_buffer_flush(rt_buffer* buffer);
rt_status rt_buffer_device_address(rt_buffer* buffer,
                                   uint64_t* output_address);
rt_status rt_buffer_get_info(rt_buffer* buffer,
                             rt_buffer_info* output_info);
/* Records an application access; it does not perform the access itself. */
rt_status rt_buffer_trace_access(rt_buffer* buffer,
                                 rt_trace_access_kind kind,
                                 size_t offset_bytes,
                                 size_t size_bytes,
                                 uint32_t stream_id);
rt_status rt_buffer_read_bytes(rt_buffer* buffer,
                               size_t offset_bytes,
                               void* output,
                               size_t size_bytes,
                               uint32_t stream_id);
rt_status rt_buffer_write_bytes(rt_buffer* buffer,
                                size_t offset_bytes,
                                const void* input,
                                size_t size_bytes,
                                uint32_t stream_id);

#ifdef __cplusplus
}
#endif

#endif
