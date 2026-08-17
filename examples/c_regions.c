#include "rtmem/rtmem.h"

#include <stdio.h>
#include <stdlib.h>

static void require(rt_runtime* runtime, rt_status status) {
    if (status != RT_OK) {
        fprintf(stderr,
                "%s: %s\n",
                rt_status_string(status),
                runtime == NULL ? "" : rt_runtime_last_error(runtime));
        exit(1);
    }
}

int main(void) {
    rt_runtime* runtime = NULL;
    require(NULL, rt_runtime_create(NULL, &runtime));

    rt_policy policy;
    rt_policy_init(&policy, RT_CLASS_EPHEMERAL);
    policy.on_expiry = RT_EXPIRY_PROMOTE;
    policy.promotion_target = RT_CLASS_EPOCH;

    rt_region* iteration = NULL;
    require(runtime,
            rt_region_create(runtime, &policy, "iteration", &iteration));

    rt_buffer* frontier = NULL;
    require(runtime, rt_alloc(iteration, 4096, 64, &frontier));

    void* pointer = NULL;
    require(runtime, rt_buffer_map(frontier, RT_MAP_WRITE, &pointer));
    ((unsigned*)pointer)[0] = 42;
    require(runtime, rt_buffer_unmap(frontier));

    uint64_t address = 0;
    require(runtime, rt_buffer_device_address(frontier, &address));
    printf("device address: 0x%llx\n", (unsigned long long)address);

    rt_buffer_free(frontier);
    rt_region_destroy(iteration);
    rt_runtime_destroy(runtime);
    return 0;
}
