#pragma once

#include "retention_model.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace rtmem {

struct DeviceProfile {
    int version = 1;
    std::string profile_id = "builtin-illustrative-v1";
    std::string source = "illustrative_not_measured";
    std::string fingerprint = "builtin";
    double tick_ns = 1.0;
    std::uint64_t line_bytes = kLineBytes;
    double controller_access_pj = 0.0;
    double metadata_access_pj = 0.0;
    double ecc_read_pj = 0.0;
    double ecc_write_pj = 0.0;
    double migration_setup_pj = 0.0;
    std::uint64_t migration_setup_cycles = 0;
    BankConfigs banks{};
    std::array<double, 3> static_power_mw{};
    std::array<std::uint32_t, 3> channels{{1, 1, 1}};
};

DeviceProfile default_device_profile();
DeviceProfile load_device_profile(const std::string& path);

}  // namespace rtmem
