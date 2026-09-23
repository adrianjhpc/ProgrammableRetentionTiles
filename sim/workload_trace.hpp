#pragma once

#include "device_profile.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace rtmem {

enum class ReplayPolicy : std::uint8_t {
    Hint,
    Ephemeral,
    Epoch,
    Durable,
    Oracle,
    Refresh,
    Adaptive,
};

struct ReplayOptions {
    std::uint64_t maintenance_guard_cycles = 8;
    bool require_safe = false;
    std::string json_path;
};

struct ReplayOutcome {
    bool completed = false;
    bool safe = false;
};

struct LifetimeAnalysisOptions {
    std::vector<std::uint64_t> thresholds{
        16, 64, 128, 512, 4096, 16384, 65536, 1048576};
    std::string csv_path;
    std::string json_path;
};

ReplayPolicy parse_replay_policy(const std::string& text);
std::string to_string(ReplayPolicy policy);
bool is_workload_trace(const std::string& path);
bool analyze_workload_lifetimes(const std::string& path,
                                const DeviceProfile& profile,
                                const LifetimeAnalysisOptions& options,
                                std::ostream& output,
                                std::ostream& errors);
ReplayOutcome run_workload_trace(const std::string& path,
                                 ReplayPolicy policy,
                                 ErrorMode mode,
                                 std::uint64_t seed,
                                 const DeviceProfile& profile,
                                 const ReplayOptions& options,
                                 std::ostream& output,
                                 std::ostream& errors);

}  // namespace rtmem
