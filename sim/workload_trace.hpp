#pragma once

#include "retention_model.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>

namespace rtmem {

enum class ReplayPolicy : std::uint8_t {
    Hint,
    Ephemeral,
    Epoch,
    Durable,
    Oracle,
};

ReplayPolicy parse_replay_policy(const std::string& text);
std::string to_string(ReplayPolicy policy);
bool is_workload_trace(const std::string& path);
bool run_workload_trace(const std::string& path,
                        ReplayPolicy policy,
                        ErrorMode mode,
                        std::uint64_t seed,
                        const BankConfigs& configs,
                        std::ostream& output,
                        std::ostream& errors);

}  // namespace rtmem
