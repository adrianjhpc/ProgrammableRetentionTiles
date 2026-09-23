#pragma once

#include "rtmem/rtmem.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rtmem::internal {

struct backend_statistics {
    std::uint64_t allocation_calls = 0;
    std::uint64_t allocation_time_ns = 0;
    std::uint64_t host_to_device_sync_calls = 0;
    std::uint64_t host_to_device_bytes = 0;
    std::uint64_t host_to_device_time_ns = 0;
    std::uint64_t device_to_host_sync_calls = 0;
    std::uint64_t device_to_host_bytes = 0;
    std::uint64_t device_to_host_time_ns = 0;
    std::uint64_t migration_calls = 0;
    std::uint64_t migration_bytes = 0;
    std::uint64_t migration_destination_allocation_time_ns = 0;
    std::uint64_t migration_submit_time_ns = 0;
    std::uint64_t migration_wait_time_ns = 0;
};

class backend_allocation {
public:
    virtual ~backend_allocation() = default;
};

class backend {
public:
    virtual ~backend() = default;

    virtual std::unique_ptr<backend_allocation> allocate(
        rt_retention_class retention_class,
        std::size_t size_bytes,
        std::size_t alignment) = 0;

    virtual void* map(backend_allocation& allocation,
                      std::uint32_t flags,
                      std::size_t size_bytes) = 0;
    virtual void unmap(backend_allocation& allocation,
                       std::uint32_t flags,
                       std::size_t size_bytes) = 0;
    virtual std::uint64_t device_address(
        const backend_allocation& allocation) const = 0;

    virtual std::unique_ptr<backend_allocation> migrate(
        backend_allocation& source,
        rt_retention_class source_class,
        rt_retention_class destination_class,
        std::size_t size_bytes,
        std::size_t alignment) = 0;
    // A null migration result means the backend reclassified in place.

    virtual void flush(backend_allocation& allocation,
                       std::size_t size_bytes) = 0;
    virtual backend_statistics statistics() const noexcept = 0;
    virtual bool supports_host_coherent_pointer() const noexcept = 0;
};

std::unique_ptr<backend> make_host_backend();
std::unique_ptr<backend> make_xrt_backend(const rt_xrt_options& options);

}  // namespace rtmem::internal
