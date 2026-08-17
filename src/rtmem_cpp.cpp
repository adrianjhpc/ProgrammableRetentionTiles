#include "rtmem/rtmem.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace rtmem {
namespace {

[[noreturn]] void raise(rt_status status, rt_runtime* runtime) {
    std::ostringstream message;
    message << rt_status_string(status);
    if (runtime != nullptr) {
        const char* detail = rt_runtime_last_error(runtime);
        if (detail != nullptr && *detail != '\0') {
            message << ": " << detail;
        }
    }
    throw error(status, message.str());
}

void check(rt_status status, rt_runtime* runtime) {
    if (status != RT_OK) {
        raise(status, runtime);
    }
}

rt_retention_class native(retention_class selected) {
    return static_cast<rt_retention_class>(selected);
}

}  // namespace

namespace detail {

struct runtime_state {
    rt_runtime* handle = nullptr;

    ~runtime_state() {
        rt_runtime_destroy(handle);
    }
};

struct region_state {
    std::shared_ptr<runtime_state> runtime;
    rt_region* handle = nullptr;

    ~region_state() {
        rt_region_destroy(handle);
    }
};

}  // namespace detail

error::error(rt_status status, const std::string& message)
    : std::runtime_error(message), status_(status) {}

runtime_options::runtime_options() {
    rt_runtime_options_init(&native);
}

runtime_options& runtime_options::clock(clock_mode selected,
                                        std::uint64_t tick_ns) {
    native.clock_mode = static_cast<rt_clock_mode>(selected);
    native.tick_ns = tick_ns;
    return *this;
}

runtime_options& runtime_options::retention(retention_class selected,
                                            std::uint64_t ticks) {
    native.class_retention_ticks[static_cast<std::size_t>(selected)] = ticks;
    return *this;
}

runtime_options& runtime_options::capacity(retention_class selected,
                                           std::uint64_t bytes) {
    native.class_capacity_bytes[static_cast<std::size_t>(selected)] = bytes;
    return *this;
}

runtime_options& runtime_options::poll_guard(std::uint64_t ticks) {
    native.poll_guard_ticks = ticks;
    return *this;
}

policy::policy(retention_class selected) {
    rt_policy_init(&native, rtmem::native(selected));
}

policy& policy::strength(guarantee selected) {
    native.guarantee = static_cast<rt_guarantee>(selected);
    return *this;
}

policy& policy::expiry(expiry_action action,
                       retention_class promotion_target) {
    native.on_expiry = static_cast<rt_expiry_action>(action);
    native.promotion_target = rtmem::native(promotion_target);
    return *this;
}

policy& policy::maximum_error_rate(double rate) {
    native.maximum_bit_error_rate = rate;
    return *this;
}

runtime::runtime() : runtime(runtime_options{}) {}

runtime::runtime(const runtime_options& options)
    : state_(std::make_shared<detail::runtime_state>()) {
    check(rt_runtime_create(&options.native, &state_->handle), nullptr);
}

rt_runtime* runtime::native_handle() const noexcept {
    return state_->handle;
}

void runtime::advance(std::uint64_t ticks) {
    check(rt_runtime_advance(state_->handle, ticks), state_->handle);
}

void runtime::poll() {
    check(rt_runtime_poll(state_->handle), state_->handle);
}

void runtime::fence() {
    check(rt_runtime_fence(state_->handle), state_->handle);
}

std::uint64_t runtime::now() const {
    std::uint64_t result = 0;
    check(rt_runtime_now(state_->handle, &result), state_->handle);
    return result;
}

rt_runtime_stats runtime::stats() const {
    rt_runtime_stats result{};
    result.struct_size = sizeof(result);
    check(rt_runtime_get_stats(state_->handle, &result), state_->handle);
    return result;
}

region::region(runtime& owner,
               const policy& selected_policy,
               std::string debug_name)
    : state_(std::make_shared<detail::region_state>()) {
    state_->runtime = owner.state_;
    check(rt_region_create(owner.native_handle(),
                           &selected_policy.native,
                           debug_name.c_str(),
                           &state_->handle),
          owner.native_handle());
}

rt_region* region::native_handle() const noexcept {
    return state_->handle;
}

void region::end() {
    check(rt_region_end(state_->handle), state_->runtime->handle);
}

void region::refresh() {
    check(rt_region_refresh(state_->handle), state_->runtime->handle);
}

void region::reclassify(retention_class destination) {
    check(rt_region_reclassify(state_->handle, native(destination)),
          state_->runtime->handle);
}

rt_region_stats region::stats() const {
    rt_region_stats result{};
    result.struct_size = sizeof(result);
    check(rt_region_get_stats(state_->handle, &result),
          state_->runtime->handle);
    return result;
}

buffer::buffer(region& owner, std::size_t bytes, std::size_t alignment)
    : region_(owner.state_) {
    check(rt_alloc(region_->handle, bytes, alignment, &handle_),
          region_->runtime->handle);
}

buffer::~buffer() {
    reset();
}

buffer::buffer(buffer&& other) noexcept
    : region_(std::move(other.region_)), handle_(other.handle_) {
    other.handle_ = nullptr;
}

buffer& buffer::operator=(buffer&& other) noexcept {
    if (this != &other) {
        reset();
        region_ = std::move(other.region_);
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void buffer::reset() noexcept {
    if (handle_ != nullptr) {
        rt_buffer_free(handle_);
        handle_ = nullptr;
    }
    region_.reset();
}

void* buffer::map(std::uint32_t flags) {
    void* result = nullptr;
    check(rt_buffer_map(handle_, flags, &result), region_->runtime->handle);
    return result;
}

void buffer::unmap() {
    check(rt_buffer_unmap(handle_), region_->runtime->handle);
}

void buffer::mark_written() {
    check(rt_buffer_mark_written(handle_), region_->runtime->handle);
}

void buffer::refresh() {
    check(rt_buffer_refresh(handle_), region_->runtime->handle);
}

void buffer::reclassify(retention_class destination) {
    check(rt_buffer_reclassify(handle_, native(destination)),
          region_->runtime->handle);
}

void buffer::promote(retention_class destination) {
    check(rt_buffer_promote(handle_, native(destination)),
          region_->runtime->handle);
}

void buffer::invalidate() {
    check(rt_buffer_invalidate(handle_), region_->runtime->handle);
}

void buffer::flush() {
    check(rt_buffer_flush(handle_), region_->runtime->handle);
}

std::uint64_t buffer::device_address() const {
    std::uint64_t result = 0;
    check(rt_buffer_device_address(handle_, &result),
          region_->runtime->handle);
    return result;
}

rt_buffer_info buffer::info() const {
    rt_buffer_info result{};
    result.struct_size = sizeof(result);
    check(rt_buffer_get_info(handle_, &result), region_->runtime->handle);
    return result;
}

struct retention_resource::impl {
    std::shared_ptr<detail::region_state> region;
    std::mutex mutex;
    std::unordered_map<void*, rt_buffer*> allocations;

    ~impl() {
        for (auto& allocation : allocations) {
            rt_buffer_free(allocation.second);
        }
    }
};

retention_resource::retention_resource(region& owner)
    : implementation_(std::make_unique<impl>()) {
    implementation_->region = owner.state_;
}

retention_resource::~retention_resource() = default;

void* retention_resource::do_allocate(std::size_t bytes,
                                      std::size_t alignment) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    rt_buffer* allocated = nullptr;
    auto* runtime = implementation_->region->runtime->handle;
    check(rt_alloc(implementation_->region->handle,
                   bytes,
                   std::max(alignment, sizeof(void*)),
                   &allocated),
          runtime);

    void* pointer = nullptr;
    const auto map_status =
        rt_buffer_map(allocated, RT_MAP_READ | RT_MAP_WRITE, &pointer);
    if (map_status != RT_OK) {
        rt_buffer_free(allocated);
        check(map_status, runtime);
    }
    const auto unmap_status = rt_buffer_unmap(allocated);
    if (unmap_status != RT_OK) {
        rt_buffer_free(allocated);
        check(unmap_status, runtime);
    }
    try {
        implementation_->allocations.emplace(pointer, allocated);
    } catch (...) {
        rt_buffer_free(allocated);
        throw;
    }
    return pointer;
}

void retention_resource::do_deallocate(void* pointer,
                                       std::size_t,
                                       std::size_t) {
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    const auto found = implementation_->allocations.find(pointer);
    if (found == implementation_->allocations.end()) {
        return;
    }
    rt_buffer_free(found->second);
    implementation_->allocations.erase(found);
}

bool retention_resource::do_is_equal(
    const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

}  // namespace rtmem
