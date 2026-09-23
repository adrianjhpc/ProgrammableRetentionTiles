#include "backend.hpp"

#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace rtmem::internal {
namespace {

constexpr std::size_t kWordBytes = 64;

std::size_t round_to_words(std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(-1) - (kWordBytes - 1)) {
        throw std::overflow_error("XRT allocation size overflow");
    }
    return ((bytes + kWordBytes - 1) / kWordBytes) * kWordBytes;
}

std::size_t class_index(rt_retention_class retention_class) {
    const auto index = static_cast<std::size_t>(retention_class);
    if (index >= RT_CLASS_COUNT) {
        throw std::invalid_argument("invalid retention class");
    }
    return index;
}

class xrt_allocation final : public backend_allocation {
public:
    xrt_allocation(const xrt::device& device,
                   std::size_t logical_size,
                   xrt::memory_group group)
        : logical_size_(logical_size),
          physical_size_(round_to_words(logical_size)),
          bo_(device,
              physical_size_,
              xrt::bo::flags::normal,
              group),
          mapped_(bo_.map<void*>()) {}

    xrt::bo& bo() noexcept { return bo_; }
    const xrt::bo& bo() const noexcept { return bo_; }
    void* mapped() noexcept { return mapped_; }
    std::size_t physical_size() const noexcept { return physical_size_; }

private:
    std::size_t logical_size_;
    std::size_t physical_size_;
    xrt::bo bo_;
    void* mapped_ = nullptr;
};

xrt_allocation& as_xrt(backend_allocation& allocation) {
    auto* result = dynamic_cast<xrt_allocation*>(&allocation);
    if (result == nullptr) {
        throw std::invalid_argument("allocation does not belong to XRT backend");
    }
    return *result;
}

const xrt_allocation& as_xrt(const backend_allocation& allocation) {
    auto* result = dynamic_cast<const xrt_allocation*>(&allocation);
    if (result == nullptr) {
        throw std::invalid_argument("allocation does not belong to XRT backend");
    }
    return *result;
}

class xrt_backend final : public backend {
public:
    explicit xrt_backend(const rt_xrt_options& options)
        : device_(open_device(options)),
          uuid_(device_.load_xclbin(options.xclbin_path)),
          kernel_(device_, uuid_, options.migration_kernel_name) {
        for (std::size_t index = 0; index < RT_CLASS_COUNT; ++index) {
            groups_[index] = kernel_.group_id(static_cast<int>(index));
            dummies_[index] = std::make_unique<xrt::bo>(
                device_, kWordBytes, xrt::bo::flags::normal, groups_[index]);
            auto* pointer = dummies_[index]->map<void*>();
            std::memset(pointer, 0, kWordBytes);
            dummies_[index]->sync(
                XCL_BO_SYNC_BO_TO_DEVICE, kWordBytes, 0);
        }
    }

    std::unique_ptr<backend_allocation> allocate(
        rt_retention_class retention_class,
        std::size_t size_bytes,
        std::size_t) override {
        const auto allocation_start = clock::now();
        auto result = std::make_unique<xrt_allocation>(
            device_, size_bytes, groups_.at(class_index(retention_class)));
        statistics_.allocation_calls++;
        statistics_.allocation_time_ns += elapsed_ns(allocation_start);

        std::memset(result->mapped(), 0, result->physical_size());
        const auto sync_start = clock::now();
        result->bo().sync(XCL_BO_SYNC_BO_TO_DEVICE,
                          result->physical_size(),
                          0);
        record_host_to_device(result->physical_size(), sync_start);
        return result;
    }

    void* map(backend_allocation& allocation,
              std::uint32_t flags,
              std::size_t size_bytes) override {
        auto& selected = as_xrt(allocation);
        if ((flags & RT_MAP_READ) != 0) {
            const auto start = clock::now();
            selected.bo().sync(
                XCL_BO_SYNC_BO_FROM_DEVICE, size_bytes, 0);
            record_device_to_host(size_bytes, start);
        }
        return selected.mapped();
    }

    void unmap(backend_allocation& allocation,
               std::uint32_t flags,
               std::size_t size_bytes) override {
        if ((flags & RT_MAP_WRITE) != 0) {
            const auto start = clock::now();
            as_xrt(allocation).bo().sync(
                XCL_BO_SYNC_BO_TO_DEVICE, size_bytes, 0);
            record_host_to_device(size_bytes, start);
        }
    }

    std::uint64_t device_address(
        const backend_allocation& allocation) const override {
        return as_xrt(allocation).bo().address();
    }

    std::unique_ptr<backend_allocation> migrate(
        backend_allocation& source,
        rt_retention_class source_class,
        rt_retention_class destination_class,
        std::size_t size_bytes,
        std::size_t alignment) override {
        (void)alignment;
        // The migration kernel overwrites every padded destination word. Do
        // not zero-fill or upload the destination first: that would charge an
        // unrelated host-to-device transfer to every promotion.
        const auto allocation_start = clock::now();
        auto destination = std::make_unique<xrt_allocation>(
            device_,
            size_bytes,
            groups_.at(class_index(destination_class)));
        statistics_.migration_destination_allocation_time_ns +=
            elapsed_ns(allocation_start);
        auto& source_xrt = as_xrt(source);
        auto& destination_xrt = as_xrt(*destination);

        std::array<xrt::bo*, RT_CLASS_COUNT> arguments{
            dummies_[0].get(), dummies_[1].get(), dummies_[2].get()};
        arguments[class_index(source_class)] = &source_xrt.bo();
        arguments[class_index(destination_class)] = &destination_xrt.bo();

        const auto submit_start = clock::now();
        auto run = kernel_(
            *arguments[0],
            *arguments[1],
            *arguments[2],
            static_cast<std::uint64_t>(
                destination_xrt.physical_size() / kWordBytes),
            static_cast<std::uint32_t>(source_class),
            static_cast<std::uint32_t>(destination_class));
        statistics_.migration_submit_time_ns += elapsed_ns(submit_start);
        const auto wait_start = clock::now();
        run.wait();
        statistics_.migration_wait_time_ns += elapsed_ns(wait_start);
        statistics_.migration_calls++;
        statistics_.migration_bytes += size_bytes;
        return destination;
    }

    void flush(backend_allocation& allocation,
               std::size_t size_bytes) override {
        const auto start = clock::now();
        as_xrt(allocation).bo().sync(
            XCL_BO_SYNC_BO_TO_DEVICE, size_bytes, 0);
        record_host_to_device(size_bytes, start);
    }

    backend_statistics statistics() const noexcept override {
        return statistics_;
    }

    bool supports_host_coherent_pointer() const noexcept override {
        return false;
    }

private:
    using clock = std::chrono::steady_clock;

    static std::uint64_t elapsed_ns(clock::time_point start) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now() - start)
                .count());
    }

    void record_host_to_device(std::size_t bytes,
                               clock::time_point start) {
        statistics_.host_to_device_sync_calls++;
        statistics_.host_to_device_bytes += bytes;
        statistics_.host_to_device_time_ns += elapsed_ns(start);
    }

    void record_device_to_host(std::size_t bytes,
                               clock::time_point start) {
        statistics_.device_to_host_sync_calls++;
        statistics_.device_to_host_bytes += bytes;
        statistics_.device_to_host_time_ns += elapsed_ns(start);
    }

    static xrt::device open_device(const rt_xrt_options& options) {
        if (options.device_bdf != nullptr && options.device_bdf[0] != '\0') {
            return xrt::device(std::string(options.device_bdf));
        }
        return xrt::device(options.device_index);
    }

    xrt::device device_;
    xrt::uuid uuid_;
    xrt::kernel kernel_;
    std::array<xrt::memory_group, RT_CLASS_COUNT> groups_{};
    std::array<std::unique_ptr<xrt::bo>, RT_CLASS_COUNT> dummies_{};
    backend_statistics statistics_{};
};

}  // namespace

std::unique_ptr<backend> make_xrt_backend(const rt_xrt_options& options) {
    return std::make_unique<xrt_backend>(options);
}

}  // namespace rtmem::internal
