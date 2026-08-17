#include "rtmem/rtmem.hpp"

#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

int main() {
    rtmem::runtime runtime;
    rtmem::policy policy{rtmem::retention_class::epoch};
    rtmem::region region{runtime, policy, "concurrency-test"};

    constexpr int thread_count = 8;
    constexpr int allocations_per_thread = 100;
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int thread = 0; thread < thread_count; ++thread) {
        workers.emplace_back([&region, thread] {
            for (int allocation = 0;
                 allocation < allocations_per_thread;
                 ++allocation) {
                rtmem::buffer buffer{region, 256, 64};
                auto* data = buffer.map_as<std::uint32_t>(RT_MAP_WRITE);
                data[0] = static_cast<std::uint32_t>(thread);
                data[1] = static_cast<std::uint32_t>(allocation);
                buffer.unmap();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto stats = runtime.stats();
    assert(stats.allocations ==
           static_cast<std::uint64_t>(thread_count * allocations_per_thread));
    assert(stats.active_buffers == 0);
    return 0;
}

