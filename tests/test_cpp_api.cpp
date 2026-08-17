#include "rtmem/rtmem.hpp"

#include <cassert>
#include <cstdint>
#include <memory_resource>
#include <numeric>
#include <vector>

int main() {
    rtmem::runtime_options options;
    options.retention(rtmem::retention_class::ephemeral, 16)
        .retention(rtmem::retention_class::epoch, 128)
        .poll_guard(4);
    rtmem::runtime runtime{options};

    rtmem::policy selected{rtmem::retention_class::ephemeral};
    selected.expiry(rtmem::expiry_action::promote,
                    rtmem::retention_class::epoch);
    rtmem::region region{runtime, selected, "cpp-api-test"};

    rtmem::buffer buffer{region, 256, 64};
    auto* words = buffer.map_as<std::uint32_t>(RT_MAP_WRITE);
    words[0] = 0x12345678u;
    buffer.unmap();
    assert(buffer.device_address() != 0);

    runtime.advance(13);
    runtime.poll();
    assert(buffer.info().retention_class == RT_CLASS_EPOCH);

    {
        rtmem::retention_resource resource{region};
        std::pmr::vector<int> values{&resource};
        values.resize(128);
        std::iota(values.begin(), values.end(), 0);
        assert(values[127] == 127);
    }

    const auto stats = runtime.stats();
    assert(stats.promotions == 1);
    assert(stats.allocations >= 2);
    return 0;
}

