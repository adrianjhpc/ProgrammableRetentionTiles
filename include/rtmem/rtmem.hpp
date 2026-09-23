#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <type_traits>

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

class trace {
public:
    explicit trace(std::string path,
                   std::uint32_t flags = RT_TRACE_DEFAULT);
    ~trace();

    trace(const trace&) = delete;
    trace& operator=(const trace&) = delete;
    trace(trace&& other) noexcept;
    trace& operator=(trace&& other) noexcept;

    void flush();
    rt_trace* native_handle() const noexcept { return handle_; }

private:
    void reset() noexcept;
    rt_trace* handle_ = nullptr;
};

class runtime {
public:
    runtime();
    explicit runtime(const runtime_options& options);
    runtime(const runtime_options& options, const xrt_options& xrt);

    rt_runtime* native_handle() const noexcept;
    void advance(std::uint64_t ticks);
    void poll();
    void fence();
    void attach_trace(trace& recorder);
    void detach_trace();
    void trace_phase(const std::string& name);
    void trace_compute(std::uint64_t cycles, std::uint32_t stream_id = 0);
    void trace_barrier(std::uint64_t barrier_id,
                       std::uint32_t stream_id = 0);
    std::uint64_t now() const;
    rt_runtime_stats stats() const;
    rt_backend_stats backend_stats() const;

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
    void trace_access(rt_trace_access_kind kind,
                      std::size_t offset_bytes,
                      std::size_t size_bytes,
                      std::uint32_t stream_id = 0);
    void read_bytes(std::size_t offset_bytes,
                    void* output,
                    std::size_t size_bytes,
                    std::uint32_t stream_id = 0);
    void write_bytes(std::size_t offset_bytes,
                     const void* input,
                     std::size_t size_bytes,
                     std::uint32_t stream_id = 0);
    rt_buffer* native_handle() const noexcept { return handle_; }

private:
    void reset() noexcept;
    std::shared_ptr<detail::region_state> region_;
    rt_buffer* handle_ = nullptr;
};

template <typename T>
class traced_view {
    static_assert(std::is_trivially_copyable<T>::value,
                  "traced_view requires a trivially copyable type");

public:
    traced_view(buffer& owner,
                std::size_t elements,
                std::uint32_t map_flags = RT_MAP_READ | RT_MAP_WRITE,
                std::uint32_t stream_id = 0)
        : owner_(&owner),
          elements_(elements),
          map_flags_(map_flags),
          stream_id_(stream_id) {
        const auto buffer_info = owner.info();
        if (elements > buffer_info.size_bytes / sizeof(T)) {
            throw std::out_of_range("traced view exceeds buffer size");
        }
        data_ = owner.map_as<T>(map_flags);
    }

    ~traced_view() {
        close_noexcept();
    }

    traced_view(const traced_view&) = delete;
    traced_view& operator=(const traced_view&) = delete;

    traced_view(traced_view&& other) noexcept
        : owner_(other.owner_),
          data_(other.data_),
          elements_(other.elements_),
          map_flags_(other.map_flags_),
          stream_id_(other.stream_id_) {
        other.owner_ = nullptr;
        other.data_ = nullptr;
        other.elements_ = 0;
    }

    traced_view& operator=(traced_view&& other) noexcept {
        if (this != &other) {
            close_noexcept();
            owner_ = other.owner_;
            data_ = other.data_;
            elements_ = other.elements_;
            map_flags_ = other.map_flags_;
            stream_id_ = other.stream_id_;
            other.owner_ = nullptr;
            other.data_ = nullptr;
            other.elements_ = 0;
        }
        return *this;
    }

    T load(std::size_t index) {
        require_index(index);
        if ((map_flags_ & RT_MAP_READ) == 0) {
            throw std::logic_error("traced view is not readable");
        }
        const T value = data_[index];
        owner_->trace_access(RT_TRACE_ACCESS_READ,
                             index * sizeof(T),
                             sizeof(T),
                             stream_id_);
        return value;
    }

    void store(std::size_t index, const T& value) {
        require_index(index);
        if ((map_flags_ & RT_MAP_WRITE) == 0) {
            throw std::logic_error("traced view is not writable");
        }
        data_[index] = value;
        owner_->trace_access(RT_TRACE_ACCESS_WRITE,
                             index * sizeof(T),
                             sizeof(T),
                             stream_id_);
    }

    void close() {
        if (owner_ != nullptr) {
            owner_->unmap();
            owner_ = nullptr;
            data_ = nullptr;
            elements_ = 0;
        }
    }

    std::size_t size() const noexcept { return elements_; }

private:
    void require_index(std::size_t index) const {
        if (owner_ == nullptr || index >= elements_) {
            throw std::out_of_range("traced view index out of range");
        }
    }

    void close_noexcept() noexcept {
        if (owner_ != nullptr) {
            try {
                owner_->unmap();
            } catch (...) {
            }
        }
        owner_ = nullptr;
        data_ = nullptr;
        elements_ = 0;
    }

    buffer* owner_ = nullptr;
    T* data_ = nullptr;
    std::size_t elements_ = 0;
    std::uint32_t map_flags_ = 0;
    std::uint32_t stream_id_ = 0;
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
