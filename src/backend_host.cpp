#include "backend.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

namespace rtmem::internal {
namespace {

class host_allocation final : public backend_allocation {
public:
    host_allocation(std::size_t size_bytes, std::size_t alignment) {
        if (posix_memalign(&data_, alignment, size_bytes) != 0) {
            throw std::bad_alloc{};
        }
        std::memset(data_, 0, size_bytes);
    }

    ~host_allocation() override {
        std::free(data_);
    }

    void* data() noexcept { return data_; }
    const void* data() const noexcept { return data_; }

private:
    void* data_ = nullptr;
};

host_allocation& as_host(backend_allocation& allocation) {
    auto* result = dynamic_cast<host_allocation*>(&allocation);
    if (result == nullptr) {
        throw std::invalid_argument("allocation does not belong to host backend");
    }
    return *result;
}

const host_allocation& as_host(const backend_allocation& allocation) {
    auto* result = dynamic_cast<const host_allocation*>(&allocation);
    if (result == nullptr) {
        throw std::invalid_argument("allocation does not belong to host backend");
    }
    return *result;
}

class host_backend final : public backend {
public:
    std::unique_ptr<backend_allocation> allocate(rt_retention_class,
                                                 std::size_t size_bytes,
                                                 std::size_t alignment) override {
        return std::make_unique<host_allocation>(size_bytes, alignment);
    }

    void* map(backend_allocation& allocation,
              std::uint32_t,
              std::size_t) override {
        return as_host(allocation).data();
    }

    void unmap(backend_allocation&,
               std::uint32_t,
               std::size_t) override {}

    std::uint64_t device_address(
        const backend_allocation& allocation) const override {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(as_host(allocation).data()));
    }

    std::unique_ptr<backend_allocation> migrate(
        backend_allocation&,
        rt_retention_class,
        rt_retention_class,
        std::size_t,
        std::size_t) override {
        // Retention classes are metadata-only in the host emulator, so the
        // allocation stays in place. A null result denotes in-place migration.
        return {};
    }

    void flush(backend_allocation&, std::size_t) override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    backend_statistics statistics() const noexcept override {
        return {};
    }

    bool supports_host_coherent_pointer() const noexcept override {
        return true;
    }
};

}  // namespace

std::unique_ptr<backend> make_host_backend() {
    return std::make_unique<host_backend>();
}

}  // namespace rtmem::internal
