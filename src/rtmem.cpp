#include "rtmem/rtmem.h"

#include "backend.hpp"
#include "trace_writer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct rt_runtime;
struct rt_region;
struct rt_buffer;

struct rt_buffer {
    rt_runtime* runtime = nullptr;
    rt_region* region = nullptr;
    std::unique_ptr<rtmem::internal::backend_allocation> allocation;
    void* exposed_pointer = nullptr;
    std::size_t size_bytes = 0;
    std::size_t alignment = 0;
    rt_retention_class retention_class = RT_CLASS_EPOCH;
    rt_guarantee guarantee = RT_GUARANTEE_REQUIRED;
    rt_expiry_action on_expiry = RT_EXPIRY_REFRESH;
    rt_retention_class promotion_target = RT_CLASS_DURABLE;
    rt_buffer_state state = RT_BUFFER_ACTIVE;
    std::uint32_t map_count = 0;
    std::uint32_t map_flags = 0;
    std::uint64_t created_tick = 0;
    std::uint64_t last_write_tick = 0;
    std::uint64_t generation = 0;
    std::uint64_t trace_id = 0;
    bool trace_live = true;
};

struct rt_trace {
    std::shared_ptr<rtmem::internal::trace_writer> writer;
};

struct rt_region {
    rt_runtime* runtime = nullptr;
    rt_policy policy{};
    std::string debug_name;
    bool active = true;
    std::vector<rt_buffer*> buffers;
    rt_region_stats stats{};
};

struct rt_runtime {
    rt_runtime_options options{};
    std::unique_ptr<rtmem::internal::backend> backend;
    mutable std::mutex mutex;
    std::chrono::steady_clock::time_point start_time;
    std::uint64_t manual_tick = 0;
    std::shared_ptr<rtmem::internal::trace_writer> trace;
    std::vector<rt_region*> regions;
    rt_runtime_stats stats{};
    std::array<char, 256> last_error{};
};

namespace {

constexpr std::uint64_t kInfinite = RTMEM_INFINITE_TICKS;
std::atomic<std::uint64_t> g_next_trace_buffer_id{1};

bool valid_class(rt_retention_class retention_class) {
    return retention_class >= RT_CLASS_EPHEMERAL &&
           retention_class < RT_CLASS_COUNT;
}

std::size_t class_index(rt_retention_class retention_class) {
    return static_cast<std::size_t>(retention_class);
}

bool valid_alignment(std::size_t alignment) {
    return alignment >= sizeof(void*) &&
           (alignment & (alignment - 1)) == 0;
}

rt_status set_error(rt_runtime* runtime,
                    rt_status status,
                    const char* message) {
    if (runtime != nullptr) {
        std::snprintf(runtime->last_error.data(),
                      runtime->last_error.size(),
                      "%s",
                      message == nullptr ? "" : message);
    }
    return status;
}

std::uint64_t now_locked(const rt_runtime* runtime) {
    if (runtime->options.clock_mode == RT_CLOCK_MANUAL) {
        return runtime->manual_tick;
    }
    const auto elapsed = std::chrono::steady_clock::now() - runtime->start_time;
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<std::uint64_t>(nanoseconds) /
           runtime->options.tick_ns;
}

std::uint64_t deadline_locked(const rt_buffer* buffer) {
    const auto retention =
        buffer->runtime->options
            .class_retention_ticks[class_index(buffer->retention_class)];
    if (retention == kInfinite ||
        buffer->last_write_tick > kInfinite - retention) {
        return kInfinite;
    }
    return buffer->last_write_tick + retention;
}

std::uint64_t total_active_bytes(const rt_runtime_stats& stats) {
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < RT_CLASS_COUNT; ++index) {
        total += stats.active_bytes[index];
    }
    return total;
}

bool trace_free_locked(rt_buffer* buffer) {
    if (!buffer->trace_live) {
        return true;
    }
    auto* runtime = buffer->runtime;
    if (runtime->trace == nullptr) {
        buffer->trace_live = false;
        return true;
    }
    if (!runtime->trace->free(now_locked(runtime), buffer->trace_id)) {
        return false;
    }
    buffer->trace_live = false;
    return true;
}

bool trace_hint_locked(rt_buffer* buffer,
                       rt_retention_class destination_class) {
    auto* runtime = buffer->runtime;
    return runtime->trace == nullptr ||
           runtime->trace->hint(now_locked(runtime),
                                buffer->trace_id,
                                destination_class);
}

void release_storage_locked(rt_buffer* buffer, rt_buffer_state new_state) {
    if (buffer->state != RT_BUFFER_ACTIVE) {
        buffer->state = new_state;
        return;
    }

    auto* runtime = buffer->runtime;
    auto* region = buffer->region;
    buffer->allocation.reset();
    buffer->exposed_pointer = nullptr;
    runtime->stats.active_buffers--;
    runtime->stats.active_bytes[class_index(buffer->retention_class)] -=
        buffer->size_bytes;
    region->stats.active_buffers--;
    region->stats.active_bytes -= buffer->size_bytes;
    buffer->state = new_state;
    buffer->map_count = 0;
    buffer->map_flags = 0;
}

rt_status validate_active_locked(rt_buffer* buffer) {
    if (buffer == nullptr || buffer->runtime == nullptr ||
        buffer->region == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    if (!buffer->region->active || buffer->state == RT_BUFFER_INVALID) {
        return set_error(buffer->runtime,
                         RT_ERROR_INVALID_STATE,
                         "buffer or owning region is inactive");
    }
    if (buffer->state == RT_BUFFER_EXPIRED) {
        return set_error(buffer->runtime,
                         RT_ERROR_EXPIRED,
                         "buffer retention guarantee has expired");
    }

    const auto deadline = deadline_locked(buffer);
    if (deadline != kInfinite && now_locked(buffer->runtime) >= deadline) {
        release_storage_locked(buffer, RT_BUFFER_EXPIRED);
        buffer->runtime->stats.expiry_events++;
        return set_error(buffer->runtime,
                         RT_ERROR_EXPIRED,
                         "buffer retention deadline was missed");
    }
    return RT_OK;
}

rt_status capacity_check_locked(rt_runtime* runtime,
                                rt_retention_class retention_class,
                                std::uint64_t bytes) {
    const auto index = class_index(retention_class);
    const auto capacity = runtime->options.class_capacity_bytes[index];
    if (capacity != 0 &&
        (bytes > capacity ||
         runtime->stats.active_bytes[index] > capacity - bytes)) {
        return set_error(runtime,
                         RT_ERROR_CAPACITY,
                         "retention class capacity exceeded");
    }
    return RT_OK;
}

rt_status reclassify_locked(rt_buffer* buffer,
                            rt_retention_class destination_class,
                            bool record_hint) {
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    if (!valid_class(destination_class)) {
        return set_error(buffer->runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "invalid destination retention class");
    }
    if (buffer->map_count != 0) {
        return set_error(buffer->runtime,
                         RT_ERROR_BUSY,
                         "cannot reclassify a mapped buffer");
    }
    if (record_hint && !trace_hint_locked(buffer, destination_class)) {
        return set_error(buffer->runtime,
                         RT_ERROR_BACKEND,
                         buffer->runtime->trace->last_error());
    }
    if (destination_class == buffer->retention_class) {
        buffer->last_write_tick = now_locked(buffer->runtime);
        buffer->generation++;
        buffer->runtime->stats.refreshes++;
        return RT_OK;
    }

    auto* runtime = buffer->runtime;
    const auto capacity =
        capacity_check_locked(runtime, destination_class, buffer->size_bytes);
    if (capacity != RT_OK) {
        return capacity;
    }

    std::unique_ptr<rtmem::internal::backend_allocation> migrated;
    try {
        migrated = runtime->backend->migrate(*buffer->allocation,
                                             buffer->retention_class,
                                             destination_class,
                                             buffer->size_bytes,
                                             buffer->alignment);
    } catch (const std::bad_alloc&) {
        return set_error(runtime,
                         RT_ERROR_NO_MEMORY,
                         "backend migration allocation failed");
    } catch (const std::exception& exception) {
        return set_error(runtime, RT_ERROR_BACKEND, exception.what());
    }

    runtime->stats.active_bytes[class_index(buffer->retention_class)] -=
        buffer->size_bytes;
    runtime->stats.active_bytes[class_index(destination_class)] +=
        buffer->size_bytes;
    runtime->stats.bytes_migrated += buffer->size_bytes;
    if (destination_class > buffer->retention_class) {
        runtime->stats.promotions++;
    } else {
        runtime->stats.demotions++;
    }
    if (migrated != nullptr) {
        buffer->allocation = std::move(migrated);
        buffer->exposed_pointer = nullptr;
    }
    buffer->retention_class = destination_class;
    buffer->last_write_tick = now_locked(runtime);
    buffer->generation++;
    return RT_OK;
}

rt_status maintain_locked(rt_buffer* buffer, std::uint64_t now) {
    if (buffer->state != RT_BUFFER_ACTIVE || buffer->map_count != 0) {
        return RT_OK;
    }
    const auto deadline = deadline_locked(buffer);
    if (deadline == kInfinite) {
        return RT_OK;
    }
    if (now >= deadline) {
        release_storage_locked(buffer, RT_BUFFER_EXPIRED);
        buffer->runtime->stats.expiry_events++;
        return set_error(buffer->runtime,
                         RT_ERROR_EXPIRED,
                         "buffer expired before maintenance");
    }

    const auto guard = buffer->runtime->options.poll_guard_ticks;
    if (guard < deadline - now) {
        return RT_OK;
    }

    switch (buffer->on_expiry) {
        case RT_EXPIRY_ERROR:
            return RT_OK;
        case RT_EXPIRY_INVALIDATE:
            release_storage_locked(buffer, RT_BUFFER_INVALID);
            buffer->runtime->stats.invalidations++;
            return RT_OK;
        case RT_EXPIRY_REFRESH:
            buffer->last_write_tick = now;
            buffer->generation++;
            buffer->runtime->stats.refreshes++;
            return RT_OK;
        case RT_EXPIRY_PROMOTE: {
            const auto result =
                reclassify_locked(buffer, buffer->promotion_target, false);
            if (result == RT_ERROR_CAPACITY &&
                buffer->guarantee != RT_GUARANTEE_REQUIRED) {
                buffer->last_write_tick = now;
                buffer->generation++;
                buffer->runtime->stats.refreshes++;
                buffer->runtime->last_error[0] = '\0';
                return RT_OK;
            }
            return result;
        }
    }
    return RT_ERROR_INVALID_ARGUMENT;
}

void end_region_locked(rt_region* region) {
    if (!region->active) {
        return;
    }
    for (auto* buffer : region->buffers) {
        trace_free_locked(buffer);
        if (buffer->state == RT_BUFFER_ACTIVE) {
            release_storage_locked(buffer, RT_BUFFER_INVALID);
            region->runtime->stats.invalidations++;
        }
    }
    region->active = false;
}

void erase_region_locked(rt_runtime* runtime, rt_region* region) {
    const auto found =
        std::find(runtime->regions.begin(), runtime->regions.end(), region);
    if (found != runtime->regions.end()) {
        runtime->regions.erase(found);
    }
}

void erase_buffer_locked(rt_region* region, rt_buffer* buffer) {
    const auto found =
        std::find(region->buffers.begin(), region->buffers.end(), buffer);
    if (found != region->buffers.end()) {
        region->buffers.erase(found);
    }
}

rt_status create_runtime(const rt_runtime_options* options,
                         const rt_xrt_options* xrt_options,
                         rt_runtime** output_runtime) {
    if (output_runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_runtime = nullptr;

    rt_runtime_options selected{};
    if (options == nullptr) {
        rt_runtime_options_init(&selected);
    } else {
        if (options->struct_size < sizeof(rt_runtime_options) ||
            options->api_version != RTMEM_API_VERSION ||
            options->tick_ns == 0) {
            return RT_ERROR_INVALID_ARGUMENT;
        }
        selected = *options;
    }
    if (selected.clock_mode != RT_CLOCK_MANUAL &&
        selected.clock_mode != RT_CLOCK_MONOTONIC) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    if (selected.backend != RT_BACKEND_HOST_EMULATED &&
        selected.backend != RT_BACKEND_XRT) {
        return RT_ERROR_UNSUPPORTED;
    }
    if (selected.backend == RT_BACKEND_XRT) {
        if (xrt_options == nullptr ||
            xrt_options->struct_size < sizeof(rt_xrt_options) ||
            xrt_options->xclbin_path == nullptr ||
            xrt_options->xclbin_path[0] == '\0' ||
            xrt_options->migration_kernel_name == nullptr ||
            xrt_options->migration_kernel_name[0] == '\0') {
            return RT_ERROR_INVALID_ARGUMENT;
        }
    }

    try {
    auto runtime = std::make_unique<rt_runtime>();
    runtime->options = selected;

    if (selected.backend == RT_BACKEND_XRT) {
        runtime->backend =
            rtmem::internal::make_xrt_backend(*xrt_options);
    } else {
        runtime->backend = rtmem::internal::make_host_backend();
    }

    runtime->start_time = std::chrono::steady_clock::now();
    runtime->stats.struct_size = sizeof(rt_runtime_stats);
    *output_runtime = runtime.release();
    return RT_OK;
} catch (const std::bad_alloc&) {
    return RT_ERROR_NO_MEMORY;
} catch (const std::exception& exception) {
    std::fprintf(stderr,
                 "XRT backend initialization failed: %s\n",
                 exception.what());
    return RT_ERROR_BACKEND;
}
}

}  // namespace

extern "C" {

void rt_runtime_options_init(rt_runtime_options* options) {
    if (options == nullptr) {
        return;
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->api_version = RTMEM_API_VERSION;
    options->backend = RT_BACKEND_HOST_EMULATED;
    options->clock_mode = RT_CLOCK_MANUAL;
    options->tick_ns = 1;
    options->poll_guard_ticks = 8;
    options->class_retention_ticks[RT_CLASS_EPHEMERAL] = 128;
    options->class_retention_ticks[RT_CLASS_EPOCH] = 4096;
    options->class_retention_ticks[RT_CLASS_DURABLE] = kInfinite;
}

void rt_xrt_options_init(rt_xrt_options* options) {
    if (options == nullptr) {
        return;
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->migration_kernel_name = "rtmem_migrate";
}

void rt_trace_options_init(rt_trace_options* options, const char* path) {
    if (options == nullptr) {
        return;
    }
    std::memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(*options);
    options->path = path;
}

rt_status rt_trace_create(const rt_trace_options* options,
                          rt_trace** output_trace) {
    if (output_trace == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_trace = nullptr;
    if (options == nullptr ||
        options->struct_size < sizeof(rt_trace_options) ||
        options->path == nullptr || options->path[0] == '\0' ||
        (options->flags & ~RT_TRACE_FLUSH_EACH_EVENT) != 0) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto trace = std::make_unique<rt_trace>();
        trace->writer = std::make_shared<rtmem::internal::trace_writer>(
            options->path, options->flags);
        *output_trace = trace.release();
        return RT_OK;
    } catch (const std::bad_alloc&) {
        return RT_ERROR_NO_MEMORY;
    } catch (const std::exception&) {
        return RT_ERROR_BACKEND;
    }
}

void rt_trace_destroy(rt_trace* trace) {
    if (trace == nullptr) {
        return;
    }
    if (trace->writer != nullptr) {
        trace->writer->flush();
    }
    delete trace;
}

const char* rt_trace_last_error(const rt_trace* trace) {
    if (trace == nullptr || trace->writer == nullptr) {
        return "invalid trace";
    }
    return trace->writer->last_error();
}

rt_status rt_trace_flush(rt_trace* trace) {
    if (trace == nullptr || trace->writer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    return trace->writer->flush() ? RT_OK : RT_ERROR_BACKEND;
}

void rt_policy_init(rt_policy* policy, rt_retention_class retention_class) {
    if (policy == nullptr) {
        return;
    }
    std::memset(policy, 0, sizeof(*policy));
    policy->struct_size = sizeof(*policy);
    policy->retention_class = retention_class;
    policy->guarantee = RT_GUARANTEE_REQUIRED;
    policy->on_expiry = RT_EXPIRY_REFRESH;
    policy->promotion_target = RT_CLASS_DURABLE;
}

const char* rt_status_string(rt_status status) {
    switch (status) {
        case RT_OK:
            return "success";
        case RT_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case RT_ERROR_NO_MEMORY:
            return "out of memory";
        case RT_ERROR_CAPACITY:
            return "retention class capacity exceeded";
        case RT_ERROR_EXPIRED:
            return "retention deadline expired";
        case RT_ERROR_INVALID_STATE:
            return "invalid state";
        case RT_ERROR_UNSUPPORTED:
            return "unsupported operation";
        case RT_ERROR_BUSY:
            return "resource busy";
        case RT_ERROR_BACKEND:
            return "backend error";
    }
    return "unknown error";
}

rt_status rt_runtime_create(const rt_runtime_options* options,
                            rt_runtime** output_runtime) {
    return create_runtime(options, nullptr, output_runtime);
}

rt_status rt_runtime_create_xrt(const rt_runtime_options* options,
                                const rt_xrt_options* xrt_options,
                                rt_runtime** output_runtime) {
    rt_runtime_options selected{};
    if (options == nullptr) {
        rt_runtime_options_init(&selected);
    } else {
        if (options->struct_size < sizeof(rt_runtime_options) ||
            options->api_version != RTMEM_API_VERSION ||
            options->tick_ns == 0) {
            if (output_runtime != nullptr) {
                *output_runtime = nullptr;
            }
            return RT_ERROR_INVALID_ARGUMENT;
        }
        selected = *options;
    }
    selected.backend = RT_BACKEND_XRT;
    return create_runtime(&selected, xrt_options, output_runtime);
}

void rt_runtime_destroy(rt_runtime* runtime) {
    if (runtime == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(runtime->mutex);
        for (auto* region : runtime->regions) {
            end_region_locked(region);
            for (auto* buffer : region->buffers) {
                delete buffer;
            }
            delete region;
        }
        runtime->regions.clear();
    }
    delete runtime;
}

const char* rt_runtime_last_error(const rt_runtime* runtime) {
    return runtime == nullptr ? "invalid runtime" : runtime->last_error.data();
}

rt_status rt_runtime_is_host_coherent(rt_runtime* runtime,
                                      uint32_t* output_supported) {
    if (runtime == nullptr || output_supported == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    *output_supported = runtime->backend->supports_host_coherent_pointer()
                            ? 1u
                            : 0u;
    return RT_OK;
}

rt_status rt_runtime_attach_trace(rt_runtime* runtime, rt_trace* trace) {
    if (runtime == nullptr || trace == nullptr || trace->writer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->trace != nullptr) {
        return set_error(runtime,
                         RT_ERROR_BUSY,
                         "runtime already has an attached trace");
    }
    runtime->trace = trace->writer;
    for (auto* region : runtime->regions) {
        for (auto* buffer : region->buffers) {
            if (!buffer->trace_live) {
                continue;
            }
            if (!runtime->trace->allocation(buffer->created_tick,
                                            buffer->trace_id,
                                            buffer->size_bytes,
                                            buffer->alignment,
                                            buffer->retention_class,
                                            region->debug_name)) {
                const auto* message = runtime->trace->last_error();
                runtime->trace.reset();
                return set_error(runtime, RT_ERROR_BACKEND, message);
            }
        }
    }
    return RT_OK;
}

rt_status rt_runtime_detach_trace(rt_runtime* runtime) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->trace != nullptr) {
        for (auto* region : runtime->regions) {
            for (auto* buffer : region->buffers) {
                if (buffer->trace_live &&
                    !runtime->trace->free(now_locked(runtime),
                                          buffer->trace_id)) {
                    return set_error(runtime,
                                     RT_ERROR_BACKEND,
                                     runtime->trace->last_error());
                }
            }
        }
        if (!runtime->trace->flush()) {
            return set_error(runtime,
                             RT_ERROR_BACKEND,
                             runtime->trace->last_error());
        }
    }
    runtime->trace.reset();
    return RT_OK;
}

rt_status rt_runtime_trace_phase(rt_runtime* runtime, const char* name) {
    if (runtime == nullptr || name == nullptr || name[0] == '\0') {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->trace == nullptr) {
        return RT_OK;
    }
    if (!runtime->trace->phase(now_locked(runtime), name)) {
        return set_error(runtime,
                         RT_ERROR_BACKEND,
                         runtime->trace->last_error());
    }
    return RT_OK;
}

rt_status rt_runtime_trace_compute(rt_runtime* runtime,
                                   uint32_t stream_id,
                                   uint64_t cycles) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->trace == nullptr) {
        return RT_OK;
    }
    if (!runtime->trace->compute(now_locked(runtime), stream_id, cycles)) {
        return set_error(runtime,
                         RT_ERROR_BACKEND,
                         runtime->trace->last_error());
    }
    return RT_OK;
}

rt_status rt_runtime_trace_barrier(rt_runtime* runtime,
                                   uint32_t stream_id,
                                   uint64_t barrier_id) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->trace == nullptr) {
        return RT_OK;
    }
    if (!runtime->trace->barrier(
            now_locked(runtime), stream_id, barrier_id)) {
        return set_error(runtime,
                         RT_ERROR_BACKEND,
                         runtime->trace->last_error());
    }
    return RT_OK;
}

rt_status rt_runtime_now(rt_runtime* runtime, uint64_t* output_tick) {
    if (runtime == nullptr || output_tick == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    *output_tick = now_locked(runtime);
    return RT_OK;
}

rt_status rt_runtime_advance(rt_runtime* runtime, uint64_t ticks) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->options.clock_mode != RT_CLOCK_MANUAL) {
        return set_error(runtime,
                         RT_ERROR_UNSUPPORTED,
                         "manual advance requires RT_CLOCK_MANUAL");
    }
    if (runtime->manual_tick > kInfinite - ticks) {
        return set_error(runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "manual clock overflow");
    }
    runtime->manual_tick += ticks;
    return RT_OK;
}

rt_status rt_runtime_poll(rt_runtime* runtime) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->stats.poll_calls++;
    const auto now = now_locked(runtime);
    rt_status first_error = RT_OK;
    for (auto* region : runtime->regions) {
        if (!region->active) {
            continue;
        }
        for (auto* buffer : region->buffers) {
            const auto status = maintain_locked(buffer, now);
            if (status != RT_OK && first_error == RT_OK) {
                first_error = status;
            }
        }
    }
    return first_error;
}

rt_status rt_runtime_fence(rt_runtime* runtime) {
    if (runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    try {
        for (auto* region : runtime->regions) {
            for (auto* buffer : region->buffers) {
                if (buffer->state == RT_BUFFER_ACTIVE &&
                    buffer->allocation != nullptr) {
                    runtime->backend->flush(*buffer->allocation,
                                            buffer->size_bytes);
                }
            }
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return RT_OK;
    } catch (const std::exception& exception) {
        return set_error(runtime, RT_ERROR_BACKEND, exception.what());
    }
}

rt_status rt_runtime_get_stats(rt_runtime* runtime,
                               rt_runtime_stats* output_stats) {
    if (runtime == nullptr || output_stats == nullptr ||
        output_stats->struct_size < sizeof(rt_runtime_stats)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    *output_stats = runtime->stats;
    output_stats->struct_size = sizeof(rt_runtime_stats);
    return RT_OK;
}

rt_status rt_region_create(rt_runtime* runtime,
                           const rt_policy* policy,
                           const char* debug_name,
                           rt_region** output_region) {
    if (runtime == nullptr || output_region == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_region = nullptr;
    rt_policy selected{};
    if (policy == nullptr) {
        rt_policy_init(&selected, RT_CLASS_EPOCH);
    } else {
        if (policy->struct_size < sizeof(rt_policy)) {
            return RT_ERROR_INVALID_ARGUMENT;
        }
        selected = *policy;
    }
    if (!valid_class(selected.retention_class) ||
        !valid_class(selected.promotion_target) ||
        selected.guarantee < RT_GUARANTEE_REQUIRED ||
        selected.guarantee > RT_GUARANTEE_APPROXIMATE ||
        selected.on_expiry < RT_EXPIRY_ERROR ||
        selected.on_expiry > RT_EXPIRY_PROMOTE ||
        selected.maximum_bit_error_rate < 0.0 ||
        selected.maximum_bit_error_rate > 1.0) {
        return set_error(runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "invalid retention policy");
    }
    if (selected.on_expiry == RT_EXPIRY_PROMOTE &&
        selected.promotion_target <= selected.retention_class) {
        return set_error(runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "promotion target must be a stronger class");
    }

    rt_region* region = nullptr;
    try {
        region = new rt_region;
        region->runtime = runtime;
        region->policy = selected;
        region->debug_name = debug_name == nullptr ? "" : debug_name;
        region->stats.struct_size = sizeof(rt_region_stats);
        std::lock_guard<std::mutex> lock(runtime->mutex);
        runtime->regions.push_back(region);
        *output_region = region;
        return RT_OK;
    } catch (const std::bad_alloc&) {
        delete region;
        return set_error(runtime, RT_ERROR_NO_MEMORY, "cannot create region");
    }
}

rt_status rt_region_end(rt_region* region) {
    if (region == nullptr || region->runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(region->runtime->mutex);
    end_region_locked(region);
    return RT_OK;
}

void rt_region_destroy(rt_region* region) {
    if (region == nullptr || region->runtime == nullptr) {
        return;
    }
    auto* runtime = region->runtime;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    end_region_locked(region);
    for (auto* buffer : region->buffers) {
        delete buffer;
    }
    region->buffers.clear();
    erase_region_locked(runtime, region);
    delete region;
}

rt_status rt_region_get_stats(rt_region* region,
                              rt_region_stats* output_stats) {
    if (region == nullptr || output_stats == nullptr ||
        output_stats->struct_size < sizeof(rt_region_stats)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(region->runtime->mutex);
    *output_stats = region->stats;
    output_stats->struct_size = sizeof(rt_region_stats);
    return RT_OK;
}

rt_status rt_region_refresh(rt_region* region) {
    if (region == nullptr || region->runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    auto* runtime = region->runtime;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!region->active) {
        return set_error(runtime,
                         RT_ERROR_INVALID_STATE,
                         "cannot refresh an ended region");
    }
    for (auto* buffer : region->buffers) {
        const auto active = validate_active_locked(buffer);
        if (active != RT_OK) {
            return active;
        }
        if (buffer->map_count != 0) {
            return set_error(runtime,
                             RT_ERROR_BUSY,
                             "cannot refresh a region with mapped buffers");
        }
    }
    const auto now = now_locked(runtime);
    for (auto* buffer : region->buffers) {
        buffer->last_write_tick = now;
        buffer->generation++;
        runtime->stats.refreshes++;
    }
    return RT_OK;
}

rt_status rt_region_reclassify(rt_region* region,
                               rt_retention_class destination_class) {
    if (region == nullptr || region->runtime == nullptr ||
        !valid_class(destination_class)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    auto* runtime = region->runtime;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!region->active) {
        return set_error(runtime,
                         RT_ERROR_INVALID_STATE,
                         "cannot reclassify an ended region");
    }

    std::uint64_t bytes_to_destination = 0;
    for (auto* buffer : region->buffers) {
        const auto active = validate_active_locked(buffer);
        if (active != RT_OK) {
            return active;
        }
        if (buffer->map_count != 0) {
            return set_error(runtime,
                             RT_ERROR_BUSY,
                             "cannot reclassify a region with mapped buffers");
        }
        if (buffer->retention_class != destination_class) {
            if (bytes_to_destination > kInfinite - buffer->size_bytes) {
                return set_error(runtime,
                                 RT_ERROR_CAPACITY,
                                 "region size overflow");
            }
            bytes_to_destination += buffer->size_bytes;
        }
    }
    const auto capacity =
        capacity_check_locked(runtime, destination_class, bytes_to_destination);
    if (capacity != RT_OK) {
        return capacity;
    }
    for (auto* buffer : region->buffers) {
        const auto status =
            reclassify_locked(buffer, destination_class, true);
        if (status != RT_OK) {
            return status;
        }
    }
    return RT_OK;
}

rt_status rt_region_malloc(rt_region* region,
                           size_t size_bytes,
                           size_t alignment,
                           void** output_pointer) {
    if (output_pointer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_pointer = nullptr;
    if (region == nullptr || region->runtime == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(region->runtime->mutex);
        if (!region->runtime->backend->supports_host_coherent_pointer()) {
            return set_error(
                region->runtime,
                RT_ERROR_UNSUPPORTED,
                "pointer allocator requires a host-coherent backend; use "
                "rt_buffer_map with XRT");
        }
    }
    rt_buffer* buffer = nullptr;
    auto status = rt_alloc(region, size_bytes, alignment, &buffer);
    if (status != RT_OK) {
        return status;
    }
    void* pointer = nullptr;
    status = rt_buffer_map(buffer, RT_MAP_READ | RT_MAP_WRITE, &pointer);
    if (status == RT_OK) {
        status = rt_buffer_unmap(buffer);
    }
    if (status != RT_OK) {
        rt_buffer_free(buffer);
        return status;
    }
    *output_pointer = pointer;
    return RT_OK;
}

rt_status rt_region_free_pointer(rt_region* region, void* pointer) {
    if (region == nullptr || region->runtime == nullptr || pointer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    auto* runtime = region->runtime;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    const auto found = std::find_if(
        region->buffers.begin(),
        region->buffers.end(),
        [pointer](const rt_buffer* buffer) {
            return buffer->exposed_pointer == pointer;
        });
    if (found == region->buffers.end()) {
        return set_error(runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "pointer does not belong to the region");
    }
    auto* buffer = *found;
    if (buffer->map_count != 0) {
        return set_error(runtime,
                         RT_ERROR_BUSY,
                         "cannot free a mapped pointer");
    }
    if (!trace_free_locked(buffer)) {
        return set_error(runtime,
                         RT_ERROR_BACKEND,
                         runtime->trace->last_error());
    }
    if (buffer->state == RT_BUFFER_ACTIVE) {
        release_storage_locked(buffer, RT_BUFFER_INVALID);
    }
    region->buffers.erase(found);
    delete buffer;
    return RT_OK;
}

rt_status rt_alloc(rt_region* region,
                   size_t size_bytes,
                   size_t alignment,
                   rt_buffer** output_buffer) {
    if (region == nullptr || output_buffer == nullptr || size_bytes == 0 ||
        !valid_alignment(alignment)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_buffer = nullptr;
    auto* runtime = region->runtime;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!region->active) {
        return set_error(runtime,
                         RT_ERROR_INVALID_STATE,
                         "cannot allocate from an ended region");
    }
    const auto capacity = capacity_check_locked(
        runtime, region->policy.retention_class, size_bytes);
    if (capacity != RT_OK) {
        runtime->stats.allocation_failures++;
        return capacity;
    }

    rt_buffer* buffer = nullptr;
    try {
        auto allocation = runtime->backend->allocate(
            region->policy.retention_class, size_bytes, alignment);
        buffer = new rt_buffer;
        buffer->runtime = runtime;
        buffer->region = region;
        buffer->allocation = std::move(allocation);
        buffer->size_bytes = size_bytes;
        buffer->alignment = alignment;
        buffer->retention_class = region->policy.retention_class;
        buffer->guarantee = region->policy.guarantee;
        buffer->on_expiry = region->policy.on_expiry;
        buffer->promotion_target = region->policy.promotion_target;
        buffer->created_tick = now_locked(runtime);
        buffer->last_write_tick = buffer->created_tick;
        buffer->trace_id =
            g_next_trace_buffer_id.fetch_add(1, std::memory_order_relaxed);
        region->buffers.push_back(buffer);

        if (runtime->trace != nullptr &&
            !runtime->trace->allocation(buffer->created_tick,
                                        buffer->trace_id,
                                        buffer->size_bytes,
                                        buffer->alignment,
                                        buffer->retention_class,
                                        region->debug_name)) {
            region->buffers.pop_back();
            const auto* message = runtime->trace->last_error();
            delete buffer;
            buffer = nullptr;
            runtime->stats.allocation_failures++;
            return set_error(runtime, RT_ERROR_BACKEND, message);
        }

        runtime->stats.allocations++;
        runtime->stats.active_buffers++;
        runtime->stats.active_bytes[class_index(buffer->retention_class)] +=
            size_bytes;
        runtime->stats.peak_active_bytes = std::max(
            runtime->stats.peak_active_bytes,
            total_active_bytes(runtime->stats));
        region->stats.allocations++;
        region->stats.active_buffers++;
        region->stats.active_bytes += size_bytes;
        region->stats.peak_active_bytes =
            std::max(region->stats.peak_active_bytes,
                     region->stats.active_bytes);
        *output_buffer = buffer;
        return RT_OK;
    } catch (const std::bad_alloc&) {
        delete buffer;
        runtime->stats.allocation_failures++;
        return set_error(runtime,
                         RT_ERROR_NO_MEMORY,
                         "cannot allocate buffer metadata");
    } catch (const std::exception& exception) {
        delete buffer;
        runtime->stats.allocation_failures++;
        return set_error(runtime, RT_ERROR_BACKEND, exception.what());
    }
}

void rt_buffer_free(rt_buffer* buffer) {
    if (buffer == nullptr || buffer->runtime == nullptr ||
        buffer->region == nullptr) {
        return;
    }
    auto* runtime = buffer->runtime;
    auto* region = buffer->region;
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (buffer->state == RT_BUFFER_ACTIVE) {
        release_storage_locked(buffer, RT_BUFFER_INVALID);
    }
    trace_free_locked(buffer);
    erase_buffer_locked(region, buffer);
    delete buffer;
}

rt_status rt_buffer_map(rt_buffer* buffer,
                        uint32_t flags,
                        void** output_pointer) {
    if (buffer == nullptr || output_pointer == nullptr ||
        (flags & (RT_MAP_READ | RT_MAP_WRITE)) == 0 ||
        (flags & ~(RT_MAP_READ | RT_MAP_WRITE)) != 0) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    *output_pointer = nullptr;
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    if (buffer->map_count != 0) {
        return set_error(buffer->runtime,
                         RT_ERROR_BUSY,
                         "buffer is already mapped");
    }
    try {
        void* pointer = buffer->runtime->backend->map(
            *buffer->allocation, flags, buffer->size_bytes);
        if (pointer == nullptr) {
            return set_error(buffer->runtime,
                             RT_ERROR_BACKEND,
                             "backend returned a null mapping");
        }
        buffer->map_count = 1;
        buffer->map_flags = flags;
        buffer->exposed_pointer = pointer;
        buffer->runtime->stats.map_calls++;
        *output_pointer = pointer;
        return RT_OK;
    } catch (const std::exception& exception) {
        return set_error(buffer->runtime, RT_ERROR_BACKEND, exception.what());
    }
}

rt_status rt_buffer_unmap(rt_buffer* buffer) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    if (buffer->state != RT_BUFFER_ACTIVE || buffer->map_count == 0) {
        return set_error(buffer->runtime,
                         RT_ERROR_INVALID_STATE,
                         "buffer is not actively mapped");
    }
    const auto map_flags = buffer->map_flags;
    try {
        buffer->runtime->backend->unmap(
            *buffer->allocation, map_flags, buffer->size_bytes);
    } catch (const std::exception& exception) {
        return set_error(buffer->runtime, RT_ERROR_BACKEND, exception.what());
    }
    if ((map_flags & RT_MAP_WRITE) != 0) {
        buffer->last_write_tick = now_locked(buffer->runtime);
        buffer->generation++;
    }
    buffer->map_count = 0;
    buffer->map_flags = 0;
    return RT_OK;
}

rt_status rt_buffer_mark_written(rt_buffer* buffer) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    buffer->last_write_tick = now_locked(buffer->runtime);
    buffer->generation++;
    return RT_OK;
}

rt_status rt_buffer_refresh(rt_buffer* buffer) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    if (buffer->map_count != 0) {
        return set_error(buffer->runtime,
                         RT_ERROR_BUSY,
                         "cannot refresh a mapped buffer");
    }
    buffer->last_write_tick = now_locked(buffer->runtime);
    buffer->generation++;
    buffer->runtime->stats.refreshes++;
    return RT_OK;
}

rt_status rt_buffer_reclassify(rt_buffer* buffer,
                               rt_retention_class destination_class) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    return reclassify_locked(buffer, destination_class, true);
}

rt_status rt_buffer_promote(rt_buffer* buffer,
                            rt_retention_class destination_class) {
    if (buffer == nullptr || !valid_class(destination_class)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    if (destination_class <= buffer->retention_class) {
        return set_error(buffer->runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "promotion target must be a stronger class");
    }
    return reclassify_locked(buffer, destination_class, true);
}

rt_status rt_buffer_invalidate(rt_buffer* buffer) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    if (buffer->map_count != 0) {
        return set_error(buffer->runtime,
                         RT_ERROR_BUSY,
                         "cannot invalidate a mapped buffer");
    }
    if (!trace_free_locked(buffer)) {
        return set_error(buffer->runtime,
                         RT_ERROR_BACKEND,
                         buffer->runtime->trace->last_error());
    }
    release_storage_locked(buffer, RT_BUFFER_INVALID);
    buffer->runtime->stats.invalidations++;
    return RT_OK;
}

rt_status rt_buffer_flush(rt_buffer* buffer) {
    if (buffer == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    try {
        buffer->runtime->backend->flush(*buffer->allocation,
                                        buffer->size_bytes);
        return RT_OK;
    } catch (const std::exception& exception) {
        return set_error(buffer->runtime, RT_ERROR_BACKEND, exception.what());
    }
}

rt_status rt_buffer_device_address(rt_buffer* buffer,
                                   uint64_t* output_address) {
    if (buffer == nullptr || output_address == nullptr) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    try {
        *output_address =
            buffer->runtime->backend->device_address(*buffer->allocation);
        return RT_OK;
    } catch (const std::exception& exception) {
        return set_error(buffer->runtime, RT_ERROR_BACKEND, exception.what());
    }
}

rt_status rt_buffer_get_info(rt_buffer* buffer,
                             rt_buffer_info* output_info) {
    if (buffer == nullptr || output_info == nullptr ||
        output_info->struct_size < sizeof(rt_buffer_info)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    if (buffer->state == RT_BUFFER_ACTIVE) {
        const auto active = validate_active_locked(buffer);
        if (active != RT_OK && active != RT_ERROR_EXPIRED) {
            return active;
        }
    }
    output_info->struct_size = sizeof(rt_buffer_info);
    output_info->size_bytes = buffer->size_bytes;
    output_info->alignment = buffer->alignment;
    output_info->retention_class = buffer->retention_class;
    output_info->guarantee = buffer->guarantee;
    output_info->state = buffer->state;
    output_info->map_count = buffer->map_count;
    output_info->created_tick = buffer->created_tick;
    output_info->last_write_tick = buffer->last_write_tick;
    output_info->deadline_tick = deadline_locked(buffer);
    output_info->generation = buffer->generation;
    output_info->device_address = 0;
    if (buffer->allocation != nullptr) {
        try {
            output_info->device_address =
                buffer->runtime->backend->device_address(*buffer->allocation);
        } catch (const std::exception& exception) {
            return set_error(buffer->runtime,
                             RT_ERROR_BACKEND,
                             exception.what());
        }
    }
    return RT_OK;
}

rt_status rt_buffer_trace_access(rt_buffer* buffer,
                                 rt_trace_access_kind kind,
                                 size_t offset_bytes,
                                 size_t size_bytes,
                                 uint32_t stream_id) {
    if (buffer == nullptr || size_bytes == 0 ||
        (kind != RT_TRACE_ACCESS_READ &&
         kind != RT_TRACE_ACCESS_WRITE)) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(buffer->runtime->mutex);
    const auto active = validate_active_locked(buffer);
    if (active != RT_OK) {
        return active;
    }
    if (offset_bytes > buffer->size_bytes ||
        size_bytes > buffer->size_bytes - offset_bytes) {
        return set_error(buffer->runtime,
                         RT_ERROR_INVALID_ARGUMENT,
                         "traced access exceeds buffer bounds");
    }
    if (buffer->runtime->trace == nullptr) {
        return RT_OK;
    }
    if (!buffer->runtime->trace->access(now_locked(buffer->runtime),
                                        stream_id,
                                        kind,
                                        buffer->trace_id,
                                        offset_bytes,
                                        size_bytes)) {
        return set_error(buffer->runtime,
                         RT_ERROR_BACKEND,
                         buffer->runtime->trace->last_error());
    }
    return RT_OK;
}

rt_status rt_buffer_read_bytes(rt_buffer* buffer,
                               size_t offset_bytes,
                               void* output,
                               size_t size_bytes,
                               uint32_t stream_id) {
    if (buffer == nullptr || output == nullptr || size_bytes == 0 ||
        offset_bytes > buffer->size_bytes ||
        size_bytes > buffer->size_bytes - offset_bytes) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    void* mapping = nullptr;
    auto status = rt_buffer_map(buffer, RT_MAP_READ, &mapping);
    if (status != RT_OK) {
        return status;
    }
    std::memcpy(output,
                static_cast<const unsigned char*>(mapping) + offset_bytes,
                size_bytes);
    status = rt_buffer_unmap(buffer);
    if (status != RT_OK) {
        return status;
    }
    return rt_buffer_trace_access(buffer,
                                  RT_TRACE_ACCESS_READ,
                                  offset_bytes,
                                  size_bytes,
                                  stream_id);
}

rt_status rt_buffer_write_bytes(rt_buffer* buffer,
                                size_t offset_bytes,
                                const void* input,
                                size_t size_bytes,
                                uint32_t stream_id) {
    if (buffer == nullptr || input == nullptr || size_bytes == 0 ||
        offset_bytes > buffer->size_bytes ||
        size_bytes > buffer->size_bytes - offset_bytes) {
        return RT_ERROR_INVALID_ARGUMENT;
    }
    void* mapping = nullptr;
    auto status = rt_buffer_map(buffer, RT_MAP_WRITE, &mapping);
    if (status != RT_OK) {
        return status;
    }
    std::memcpy(static_cast<unsigned char*>(mapping) + offset_bytes,
                input,
                size_bytes);
    status = rt_buffer_unmap(buffer);
    if (status != RT_OK) {
        return status;
    }
    return rt_buffer_trace_access(buffer,
                                  RT_TRACE_ACCESS_WRITE,
                                  offset_bytes,
                                  size_bytes,
                                  stream_id);
}

}  // extern "C"
