#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>

#include "rtmem/rtmem.h"

namespace rtmem {

enum class retention_class : int {
    ephemeral = RT_CLASS_EPHEMERAL,
    epoch = RT_CLASS_EPOCH,
    durable = RT_CLASS_DURABLE,
};

enum class guarantee : int {
    required = RT_GUARANTEE_REQUIRED,
    preferred = RT_GUARANTEE_PREFERRED,
    approximate = RT_GUARANTEE_APPROXIMATE,
};

enum class expiry_action : int {
    error = RT_EXPIRY_ERROR,
    invalidate = RT_EXPIRY_INVALIDATE,
    refresh = RT_EXPIRY_REFRESH,
    promote = RT_EXPIRY_PROMOTE,
};

enum class clock_mode : int {
    manual = RT_CLOCK_MANUAL,
    monotonic = RT_CLOCK_MONOTONIC,
};

class error : public std::runtime_error {
public:
    error(rt_status status, const std::string& message);
    rt_status status() const noexcept { return status_; }

private:
    rt_status status_;
};

struct runtime_options {
    rt_runtime_options native;

    runtime_options();
    runtime_options& clock(clock_mode selected, std::uint64_t tick_ns = 1);
    runtime_options& retention(retention_class selected, std::uint64_t ticks);
    runtime_options& capacity(retention_class selected, std::uint64_t bytes);
    runtime_options& poll_guard(std::uint64_t ticks);
};

struct xrt_options {
    std::uint32_t device_index = 0;
    std::string device_bdf;
    std::string xclbin_path;
    std::string migration_kernel_name = "rtmem_migrate";
};

struct policy {
    rt_policy native;

    explicit policy(retention_class selected = retention_class::epoch);
    policy& strength(guarantee selected);
    policy& expiry(expiry_action action,
                   retention_class promotion_target =
                       retention_class::durable);
    policy& maximum_error_rate(double rate);
};

namespace detail {
struct runtime_state;
struct region_state;
}  // namespace detail

class runtime {
public:
    runtime();
    explicit runtime(const runtime_options& options);
    runtime(const runtime_options& options, const xrt_options& xrt);

    rt_runtime* native_handle() const noexcept;
    void advance(std::uint64_t ticks);
    void poll();
    void fence();
    std::uint64_t now() const;
    rt_runtime_stats stats() const;

private:
    std::shared_ptr<detail::runtime_state> state_;
    friend class region;
};

class region {
public:
    region(runtime& owner,
           const policy& selected_policy,
           std::string debug_name = {});

    rt_region* native_handle() const noexcept;
    void end();
    void refresh();
    void reclassify(retention_class destination);
    rt_region_stats stats() const;

private:
    std::shared_ptr<detail::region_state> state_;
    friend class buffer;
    friend class retention_resource;
};

class buffer {
public:
    buffer(region& owner,
           std::size_t bytes,
           std::size_t alignment = alignof(std::max_align_t));
    ~buffer();

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
    buffer(buffer&& other) noexcept;
    buffer& operator=(buffer&& other) noexcept;

    void* map(std::uint32_t flags = RT_MAP_READ | RT_MAP_WRITE);

    template <typename T>
    T* map_as(std::uint32_t flags = RT_MAP_READ | RT_MAP_WRITE) {
        return static_cast<T*>(map(flags));
    }

    void unmap();
    void mark_written();
    void refresh();
    void reclassify(retention_class destination);
    void promote(retention_class destination);
    void invalidate();
    void flush();
    std::uint64_t device_address() const;
    rt_buffer_info info() const;
    rt_buffer* native_handle() const noexcept { return handle_; }

private:
    void reset() noexcept;
    std::shared_ptr<detail::region_state> region_;
    rt_buffer* handle_ = nullptr;
};

class retention_resource final : public std::pmr::memory_resource {
public:
    explicit retention_resource(region& owner);
    ~retention_resource() override;

    retention_resource(const retention_resource&) = delete;
    retention_resource& operator=(const retention_resource&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> implementation_;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer,
                       std::size_t bytes,
                       std::size_t alignment) override;
    bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override;
};

}  // namespace rtmem
