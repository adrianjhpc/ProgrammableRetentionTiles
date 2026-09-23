#include "rtmem/rtmem.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void check(rt_status status) {
    assert(status == RT_OK);
}

int main(void) {
    rt_runtime_options options;
    rt_runtime_options_init(&options);
    options.class_retention_ticks[RT_CLASS_EPHEMERAL] = 8;
    options.class_retention_ticks[RT_CLASS_EPOCH] = 32;
    options.poll_guard_ticks = 2;

    rt_runtime* runtime = NULL;
    check(rt_runtime_create(&options, &runtime));
    uint32_t host_coherent = 0;
    check(rt_runtime_is_host_coherent(runtime, &host_coherent));
    assert(host_coherent == 1);

    rt_xrt_options xrt_options;
    rt_xrt_options_init(&xrt_options);
    assert(xrt_options.struct_size == sizeof(xrt_options));
    assert(strcmp(xrt_options.migration_kernel_name, "rtmem_migrate") == 0);

    rt_policy policy;
    rt_policy_init(&policy, RT_CLASS_EPHEMERAL);
    policy.on_expiry = RT_EXPIRY_REFRESH;

    rt_region* region = NULL;
    check(rt_region_create(runtime, &policy, "c-api-test", &region));

    rt_buffer* buffer = NULL;
    check(rt_alloc(region, 256, 64, &buffer));

    void* mapped = NULL;
    check(rt_buffer_map(buffer, RT_MAP_WRITE, &mapped));
    memset(mapped, 0xa5, 256);
    check(rt_buffer_unmap(buffer));

    check(rt_runtime_advance(runtime, 7));
    check(rt_runtime_poll(runtime));

    rt_buffer_info info = {0};
    info.struct_size = sizeof(info);
    check(rt_buffer_get_info(buffer, &info));
    assert(info.state == RT_BUFFER_ACTIVE);
    assert(info.generation == 2);

    check(rt_buffer_promote(buffer, RT_CLASS_EPOCH));
    info.struct_size = sizeof(info);
    check(rt_buffer_get_info(buffer, &info));
    assert(info.retention_class == RT_CLASS_EPOCH);
    assert(info.device_address != 0);

    check(rt_buffer_map(buffer, RT_MAP_READ, &mapped));
    assert(((const unsigned char*)mapped)[0] == 0xa5);
    check(rt_buffer_unmap(buffer));
    check(rt_buffer_invalidate(buffer));
    assert(rt_buffer_map(buffer, RT_MAP_READ, &mapped) ==
           RT_ERROR_INVALID_STATE);
    rt_buffer_free(buffer);
    rt_region_destroy(region);

    rt_policy_init(&policy, RT_CLASS_EPHEMERAL);
    policy.on_expiry = RT_EXPIRY_ERROR;
    check(rt_region_create(runtime, &policy, "expiry-test", &region));
    check(rt_alloc(region, 64, 64, &buffer));
    check(rt_runtime_advance(runtime, 8));
    assert(rt_buffer_map(buffer, RT_MAP_READ, &mapped) == RT_ERROR_EXPIRED);
    info.struct_size = sizeof(info);
    check(rt_buffer_get_info(buffer, &info));
    assert(info.state == RT_BUFFER_EXPIRED);
    rt_buffer_free(buffer);
    rt_region_destroy(region);

    rt_runtime_stats stats = {0};
    stats.struct_size = sizeof(stats);
    check(rt_runtime_get_stats(runtime, &stats));
    assert(stats.allocations == 2);
    assert(stats.refreshes == 1);
    assert(stats.promotions == 1);
    assert(stats.expiry_events == 1);
    assert(stats.active_buffers == 0);

    rt_backend_stats backend_stats = {0};
    backend_stats.struct_size = sizeof(backend_stats);
    check(rt_runtime_get_backend_stats(runtime, &backend_stats));
    assert(backend_stats.struct_size == sizeof(backend_stats));

    rt_policy_init(&policy, RT_CLASS_EPOCH);
    check(rt_region_create(runtime, &policy, "pointer-api", &region));
    void* ordinary_pointer = NULL;
    check(rt_region_malloc(region, 128, 64, &ordinary_pointer));
    memset(ordinary_pointer, 0x5a, 128);
    check(rt_region_refresh(region));
    check(rt_region_reclassify(region, RT_CLASS_DURABLE));
    check(rt_region_free_pointer(region, ordinary_pointer));
    rt_region_destroy(region);

    rt_trace_options trace_options;
    rt_trace_options_init(&trace_options, "build/test_c_api.rttrace");
    rt_trace* trace = NULL;
    check(rt_trace_create(&trace_options, &trace));
    check(rt_runtime_attach_trace(runtime, trace));
    assert(rt_runtime_attach_trace(runtime, trace) == RT_ERROR_BUSY);

    rt_policy_init(&policy, RT_CLASS_EPHEMERAL);
    check(rt_region_create(runtime, &policy, "c-trace-api", &region));
    check(rt_alloc(region, 64, 64, &buffer));
    const uint64_t traced_value = UINT64_C(0x123456789abcdef0);
    uint64_t traced_read = 0;
    check(rt_buffer_write_bytes(
        buffer, 0, &traced_value, sizeof(traced_value), 3));
    check(rt_runtime_trace_compute(runtime, 3, 5));
    check(rt_buffer_read_bytes(
        buffer, 0, &traced_read, sizeof(traced_read), 3));
    assert(traced_read == traced_value);
    check(rt_runtime_trace_phase(runtime, "c_api_complete"));
    check(rt_runtime_trace_barrier(runtime, 3, 1));
    rt_buffer_free(buffer);
    rt_region_destroy(region);
    check(rt_runtime_detach_trace(runtime));
    check(rt_trace_flush(trace));
    rt_trace_destroy(trace);

    rt_runtime_destroy(runtime);
    return 0;
}
