#pragma once

#include "device_profile.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

namespace rtmem {

enum class EvidenceFailure : std::uint8_t {
    None,
    Capacity,
    Missing,
    Uninitialized,
    Expired,
};

struct EvidenceOperation {
    bool ok = false;
    EvidenceFailure failure = EvidenceFailure::None;
    std::uint64_t start_cycle = 0;
    std::uint64_t completion_cycle = 0;
};

struct EvidenceLineStatus {
    bool present = false;
    bool initialized = false;
    bool failed = false;
    RetentionClass retention_class = RetentionClass::Epoch;
    std::uint64_t last_write_cycle = 0;
    std::uint64_t deadline_cycle = 0;
};

struct EvidenceEnergy {
    double useful_read_pj = 0.0;
    double useful_write_pj = 0.0;
    double refresh_pj = 0.0;
    double migration_pj = 0.0;
    double controller_pj = 0.0;
    double metadata_pj = 0.0;
    double ecc_pj = 0.0;
    double static_pj = 0.0;

    double total() const;
};

struct EvidenceModelStatistics {
    std::uint64_t read_attempts = 0;
    std::uint64_t write_attempts = 0;
    std::uint64_t useful_reads = 0;
    std::uint64_t useful_writes = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t migrations = 0;
    std::uint64_t expirations = 0;
    std::uint64_t uninitialized_reads = 0;
    std::uint64_t missing_accesses = 0;
    std::uint64_t capacity_failures = 0;
    std::uint64_t queue_delay_cycles = 0;
    std::array<std::uint64_t, 3> bank_busy_cycles{};
    EvidenceEnergy energy;
};

class EvidenceMemory {
public:
    EvidenceMemory(const DeviceProfile& profile,
                   ErrorMode error_mode,
                   std::uint64_t seed);

    bool reserve_range(std::uint64_t byte_address,
                       std::uint64_t line_count,
                       RetentionClass retention_class);
    void release_range(std::uint64_t byte_address,
                       std::uint64_t line_count);
    bool can_fit(RetentionClass retention_class,
                 std::uint64_t additional_lines) const;

    EvidenceOperation read(std::uint64_t byte_address,
                           std::uint64_t issue_cycle);
    EvidenceOperation write(std::uint64_t byte_address,
                            RetentionClass retention_class,
                            std::uint64_t issue_cycle);
    EvidenceOperation refresh(std::uint64_t byte_address,
                              std::uint64_t issue_cycle);
    EvidenceOperation migrate(std::uint64_t byte_address,
                              RetentionClass destination_class,
                              std::uint64_t issue_cycle);

    std::optional<EvidenceLineStatus> line_status(
        std::uint64_t byte_address) const;
    std::uint64_t earliest_start_cycle(RetentionClass retention_class,
                                       std::uint64_t issue_cycle) const;
    std::uint64_t makespan_cycles() const;
    std::uint64_t bank_ready_cycle() const;
    const EvidenceModelStatistics& statistics() const;
    EvidenceEnergy final_energy(std::uint64_t makespan_cycles) const;

private:
    struct Line {
        bool initialized = false;
        bool failed = false;
        RetentionClass retention_class = RetentionClass::Epoch;
        std::uint64_t last_write_cycle = 0;
        std::uint64_t last_retention_check = 0;
    };

    struct BankState {
        std::uint64_t used_lines = 0;
        std::vector<std::uint64_t> channel_ready;
    };

    struct Scheduled {
        std::uint64_t start = 0;
        std::uint64_t completion = 0;
    };

    static std::size_t class_index(RetentionClass retention_class);
    static std::uint64_t logical_line(std::uint64_t byte_address);
    Scheduled schedule(RetentionClass retention_class,
                       std::uint64_t issue_cycle,
                       std::uint64_t latency_cycles);
    EvidenceOperation failure(EvidenceFailure failure,
                              std::uint64_t issue_cycle) const;
    bool retention_failed(Line& line,
                          const BankConfig& config,
                          std::uint64_t observed_cycle);
    void account_control(bool read, bool write);

    DeviceProfile profile_;
    ErrorMode error_mode_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::array<BankState, 3> banks_;
    std::unordered_map<std::uint64_t, Line> lines_;
    EvidenceModelStatistics stats_;
    std::uint64_t makespan_cycles_ = 0;
};

const char* to_string(EvidenceFailure failure);

}  // namespace rtmem
