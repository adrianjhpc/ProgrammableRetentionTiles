#include "rtmem/rtmem.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t kBytes = 1u << 20;

std::uint64_t expected_word(std::size_t index) {
    return 0x9e3779b97f4a7c15ULL ^
           (static_cast<std::uint64_t>(index) * 0x100000001b3ULL);
}

void verify(rtmem::buffer& buffer) {
    const auto* words = buffer.map_as<const std::uint64_t>(RT_MAP_READ);
    for (std::size_t index = 0; index < kBytes / sizeof(*words); ++index) {
        if (words[index] != expected_word(index)) {
            buffer.unmap();
            throw std::runtime_error("data mismatch after HBM migration");
        }
    }
    buffer.unmap();
}

void print_location(const char* label, const rtmem::buffer& buffer) {
    const auto info = buffer.info();
    std::cout << label << ": class=" << info.retention_class
              << " address=0x" << std::hex << info.device_address << std::dec
              << " generation=" << info.generation << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: " << argv[0]
                  << " <rtmem_u280.xclbin> [device-index-or-bdf]\n";
        return EXIT_FAILURE;
    }

    try {
        rtmem::runtime_options runtime_options;
        runtime_options.clock(rtmem::clock_mode::manual);

        rtmem::xrt_options xrt;
        xrt.xclbin_path = argv[1];
        if (argc == 3) {
            const std::string selected = argv[2];
            if (selected.find(':') != std::string::npos) {
                xrt.device_bdf = selected;
            } else {
                xrt.device_index = static_cast<std::uint32_t>(
                    std::stoul(selected));
            }
        }

        rtmem::runtime runtime(runtime_options, xrt);
        rtmem::policy policy(rtmem::retention_class::ephemeral);
        rtmem::region region(runtime, policy, "u280-vertical-slice");
        rtmem::buffer buffer(region, kBytes, 64);

        auto* words = buffer.map_as<std::uint64_t>(RT_MAP_WRITE);
        for (std::size_t index = 0; index < kBytes / sizeof(*words); ++index) {
            words[index] = expected_word(index);
        }
        buffer.unmap();
        print_location("allocated", buffer);

        buffer.promote(rtmem::retention_class::epoch);
        print_location("promoted to epoch", buffer);
        verify(buffer);

        buffer.promote(rtmem::retention_class::durable);
        print_location("promoted to durable", buffer);
        verify(buffer);

        const auto stats = runtime.stats();
        std::cout << "verified " << kBytes << " bytes across "
                  << stats.promotions << " migrations ("
                  << stats.bytes_migrated << " bytes copied)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
