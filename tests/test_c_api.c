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

    rt_policy_init(&policy, RT_CLASS_EPOCH);
    check(rt_region_create(runtime, &policy, "pointer-api", &region));
    void* ordinary_pointer = NULL;
    check(rt_region_malloc(region, 128, 64, &ordinary_pointer));
    memset(ordinary_pointer, 0x5a, 128);
    check(rt_region_refresh(region));
    check(rt_region_reclassify(region, RT_CLASS_DURABLE));
    check(rt_region_free_pointer(region, ordinary_pointer));
    rt_region_destroy(region);

    rt_runtime_destroy(runtime);
    return 0;
}
