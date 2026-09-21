#pragma once

#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtmem {

constexpr std::uint64_t kLineBytes = 64;

enum class RetentionClass : std::uint8_t {
    Ephemeral = 0,
    Epoch = 1,
    Durable = 2,
};

enum class ErrorMode : std::uint8_t {
    Deterministic,
    Stochastic,
};

struct BankConfig {
    std::string name;
    std::size_t capacity_lines = 0;
    std::uint64_t retention_cycles = 0;
    std::uint64_t read_cycles = 0;
    std::uint64_t write_cycles = 0;
    double read_energy_pj = 0.0;
    double write_energy_pj = 0.0;
};

using BankConfigs = std::array<BankConfig, 3>;

struct Result {
    bool ok = false;
    bool expired = false;
    std::uint64_t value = 0;
    std::uint64_t latency_cycles = 0;
    std::string message;
};

struct Statistics {
    std::uint64_t reads = 0;
    std::uint64_t read_hits = 0;
    std::uint64_t read_misses = 0;
    std::uint64_t writes = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t migrations = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t expirations = 0;
    std::uint64_t capacity_failures = 0;
    double energy_pj = 0.0;
};

class RetentionMemory {
public:
    explicit RetentionMemory(ErrorMode mode = ErrorMode::Deterministic,
                             std::uint64_t seed = 1);
    RetentionMemory(const BankConfigs& configs,
                    ErrorMode mode = ErrorMode::Deterministic,
                    std::uint64_t seed = 1);

    Result write(std::uint64_t byte_address,
                 RetentionClass requested_class,
                 std::uint64_t value);
    Result read(std::uint64_t byte_address);
    Result refresh(std::uint64_t byte_address);
    Result migrate(std::uint64_t byte_address,
                   RetentionClass destination_class);
    Result invalidate(std::uint64_t byte_address);

    void advance(std::uint64_t cycles);
    std::uint64_t now() const { return now_; }
    const Statistics& statistics() const { return stats_; }
    const BankConfig& bank_config(RetentionClass retention_class) const;
    void print_statistics(std::ostream& output) const;

private:
    struct Line {
        bool valid = false;
        bool retention_failed = false;
        std::uint64_t logical_line = 0;
        std::uint64_t value = 0;
        std::uint64_t last_write_cycle = 0;
        std::uint64_t last_retention_check = 0;
    };

    struct Bank {
        BankConfig config;
        std::vector<Line> lines;
    };

    struct Location {
        RetentionClass retention_class;
        std::size_t slot = 0;
    };

    static std::size_t class_index(RetentionClass retention_class);
    static std::uint64_t logical_line(std::uint64_t byte_address);

    std::optional<std::size_t> allocate_slot(RetentionClass retention_class);
    void release(const Location& location);
    bool retention_failed(Line& line, const BankConfig& config);
    Result missing_result(const std::string& message, bool expired = false);

    std::array<Bank, 3> banks_;
    std::unordered_map<std::uint64_t, Location> directory_;
    ErrorMode error_mode_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::uint64_t now_ = 0;
    Statistics stats_;
};

std::string to_string(RetentionClass retention_class);
RetentionClass parse_retention_class(const std::string& text);
BankConfigs default_bank_configs();

}  // namespace rtmem
