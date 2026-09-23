#include "evidence_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rtmem {
namespace {

constexpr std::uint64_t kInfinite =
    std::numeric_limits<std::uint64_t>::max();

std::uint64_t checked_add(std::uint64_t left,
                          std::uint64_t right,
                          const char* message) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        throw std::overflow_error(message);
    }
    return left + right;
}

}  // namespace

double EvidenceEnergy::total() const {
    return useful_read_pj + useful_write_pj + refresh_pj + migration_pj +
           controller_pj + metadata_pj + ecc_pj + static_pj;
}

EvidenceMemory::EvidenceMemory(const DeviceProfile& profile,
                               ErrorMode error_mode,
                               std::uint64_t seed)
    : profile_(profile), error_mode_(error_mode), rng_(seed) {
    for (std::size_t index = 0; index < banks_.size(); ++index) {
        banks_[index].channel_ready.assign(profile_.channels[index], 0);
    }
}

std::size_t EvidenceMemory::class_index(RetentionClass retention_class) {
    return static_cast<std::size_t>(retention_class);
}

std::uint64_t EvidenceMemory::logical_line(std::uint64_t byte_address) {
    return byte_address / kLineBytes;
}

bool EvidenceMemory::can_fit(RetentionClass retention_class,
                             std::uint64_t additional_lines) const {
    const auto selected = class_index(retention_class);
    const auto capacity = profile_.banks[selected].capacity_lines;
    return additional_lines <= capacity &&
           banks_[selected].used_lines <= capacity - additional_lines;
}

bool EvidenceMemory::reserve_range(std::uint64_t byte_address,
                                   std::uint64_t line_count,
                                   RetentionClass retention_class) {
    if (line_count == 0 || !can_fit(retention_class, line_count)) {
        ++stats_.capacity_failures;
        return false;
    }
    const auto first = logical_line(byte_address);
    for (std::uint64_t offset = 0; offset < line_count; ++offset) {
        if (lines_.count(first + offset) != 0) {
            throw std::runtime_error("overlapping logical allocation");
        }
    }
    for (std::uint64_t offset = 0; offset < line_count; ++offset) {
        Line line;
        line.retention_class = retention_class;
        lines_.emplace(first + offset, line);
    }
    banks_[class_index(retention_class)].used_lines += line_count;
    return true;
}

void EvidenceMemory::release_range(std::uint64_t byte_address,
                                   std::uint64_t line_count) {
    const auto first = logical_line(byte_address);
    for (std::uint64_t offset = 0; offset < line_count; ++offset) {
        const auto found = lines_.find(first + offset);
        if (found == lines_.end()) {
            continue;
        }
        auto& used = banks_[class_index(found->second.retention_class)].used_lines;
        if (used == 0) {
            throw std::runtime_error("bank usage underflow");
        }
        --used;
        lines_.erase(found);
    }
}

EvidenceMemory::Scheduled EvidenceMemory::schedule(
    RetentionClass retention_class,
    std::uint64_t issue_cycle,
    std::uint64_t latency_cycles) {
    auto& bank = banks_[class_index(retention_class)];
    auto selected = std::min_element(bank.channel_ready.begin(),
                                     bank.channel_ready.end());
    const auto start = std::max(issue_cycle, *selected);
    const auto completion = checked_add(start,
                                        latency_cycles,
                                        "scheduled cycle overflow");
    stats_.queue_delay_cycles = checked_add(
        stats_.queue_delay_cycles,
        start - issue_cycle,
        "queue-delay counter overflow");
    stats_.bank_busy_cycles[class_index(retention_class)] = checked_add(
        stats_.bank_busy_cycles[class_index(retention_class)],
        latency_cycles,
        "bank-busy counter overflow");
    *selected = completion;
    makespan_cycles_ = std::max(makespan_cycles_, completion);
    return {start, completion};
}

EvidenceOperation EvidenceMemory::failure(EvidenceFailure selected,
                                          std::uint64_t issue_cycle) const {
    return {false, selected, issue_cycle, issue_cycle};
}

bool EvidenceMemory::retention_failed(Line& line,
                                      const BankConfig& config,
                                      std::uint64_t observed_cycle) {
    if (!line.initialized || line.failed ||
        config.retention_cycles == kInfinite) {
        return line.failed;
    }
    if (observed_cycle < line.last_write_cycle) {
        throw std::runtime_error("retention observation precedes write");
    }
    const auto age = observed_cycle - line.last_write_cycle;
    if (error_mode_ == ErrorMode::Deterministic) {
        line.failed = age >= config.retention_cycles;
        return line.failed;
    }

    const auto checked_from =
        std::max(line.last_retention_check, line.last_write_cycle);
    const auto elapsed = observed_cycle - checked_from;
    line.last_retention_check = observed_cycle;
    if (elapsed == 0) {
        return false;
    }
    const auto probability =
        1.0 - std::exp(-static_cast<double>(elapsed) /
                       static_cast<double>(config.retention_cycles));
    line.failed = uniform_(rng_) < probability;
    return line.failed;
}

void EvidenceMemory::account_control(bool read, bool write) {
    stats_.energy.controller_pj += profile_.controller_access_pj;
    stats_.energy.metadata_pj += profile_.metadata_access_pj;
    if (read) {
        stats_.energy.ecc_pj += profile_.ecc_read_pj;
    }
    if (write) {
        stats_.energy.ecc_pj += profile_.ecc_write_pj;
    }
}

EvidenceOperation EvidenceMemory::read(std::uint64_t byte_address,
                                       std::uint64_t issue_cycle) {
    ++stats_.read_attempts;
    const auto found = lines_.find(logical_line(byte_address));
    if (found == lines_.end()) {
        ++stats_.missing_accesses;
        return failure(EvidenceFailure::Missing, issue_cycle);
    }
    auto& line = found->second;
    if (!line.initialized) {
        ++stats_.uninitialized_reads;
        return failure(EvidenceFailure::Uninitialized, issue_cycle);
    }
    const auto& config = profile_.banks[class_index(line.retention_class)];
    const auto scheduled = schedule(line.retention_class,
                                    issue_cycle,
                                    config.read_cycles);
    if (retention_failed(line, config, scheduled.start)) {
        ++stats_.expirations;
        return failure(EvidenceFailure::Expired, scheduled.start);
    }
    ++stats_.useful_reads;
    stats_.energy.useful_read_pj += config.read_energy_pj;
    account_control(true, false);
    return {true,
            EvidenceFailure::None,
            scheduled.start,
            scheduled.completion};
}

EvidenceOperation EvidenceMemory::write(std::uint64_t byte_address,
                                        RetentionClass retention_class,
                                        std::uint64_t issue_cycle) {
    ++stats_.write_attempts;
    const auto found = lines_.find(logical_line(byte_address));
    if (found == lines_.end()) {
        ++stats_.missing_accesses;
        return failure(EvidenceFailure::Missing, issue_cycle);
    }
    auto& line = found->second;
    if (line.retention_class != retention_class) {
        throw std::runtime_error("write class disagrees with reserved line");
    }
    const auto& config = profile_.banks[class_index(retention_class)];
    const auto scheduled = schedule(retention_class,
                                    issue_cycle,
                                    config.write_cycles);
    line.initialized = true;
    line.failed = false;
    line.last_write_cycle = scheduled.completion;
    line.last_retention_check = scheduled.completion;
    ++stats_.useful_writes;
    stats_.energy.useful_write_pj += config.write_energy_pj;
    account_control(false, true);
    return {true,
            EvidenceFailure::None,
            scheduled.start,
            scheduled.completion};
}

EvidenceOperation EvidenceMemory::refresh(std::uint64_t byte_address,
                                          std::uint64_t issue_cycle) {
    const auto found = lines_.find(logical_line(byte_address));
    if (found == lines_.end()) {
        ++stats_.missing_accesses;
        return failure(EvidenceFailure::Missing, issue_cycle);
    }
    auto& line = found->second;
    if (!line.initialized) {
        return {true, EvidenceFailure::None, issue_cycle, issue_cycle};
    }
    const auto& config = profile_.banks[class_index(line.retention_class)];
    const auto latency = checked_add(config.read_cycles,
                                     config.write_cycles,
                                     "refresh latency overflow");
    const auto scheduled = schedule(line.retention_class,
                                    issue_cycle,
                                    latency);
    if (retention_failed(line, config, scheduled.start)) {
        ++stats_.expirations;
        return failure(EvidenceFailure::Expired, scheduled.start);
    }
    line.failed = false;
    line.last_write_cycle = scheduled.completion;
    line.last_retention_check = scheduled.completion;
    ++stats_.refreshes;
    stats_.energy.refresh_pj +=
        config.read_energy_pj + config.write_energy_pj;
    account_control(true, true);
    return {true,
            EvidenceFailure::None,
            scheduled.start,
            scheduled.completion};
}

EvidenceOperation EvidenceMemory::migrate(
    std::uint64_t byte_address,
    RetentionClass destination_class,
    std::uint64_t issue_cycle) {
    const auto found = lines_.find(logical_line(byte_address));
    if (found == lines_.end()) {
        ++stats_.missing_accesses;
        return failure(EvidenceFailure::Missing, issue_cycle);
    }
    auto& line = found->second;
    const auto source_class = line.retention_class;
    if (source_class == destination_class) {
        return refresh(byte_address, issue_cycle);
    }
    if (!can_fit(destination_class, 1)) {
        ++stats_.capacity_failures;
        return failure(EvidenceFailure::Capacity, issue_cycle);
    }

    if (!line.initialized) {
        --banks_[class_index(source_class)].used_lines;
        ++banks_[class_index(destination_class)].used_lines;
        line.retention_class = destination_class;
        line.failed = false;
        stats_.energy.controller_pj += profile_.controller_access_pj;
        stats_.energy.metadata_pj += profile_.metadata_access_pj;
        return {true, EvidenceFailure::None, issue_cycle, issue_cycle};
    }

    const auto& source = profile_.banks[class_index(source_class)];
    const auto& destination = profile_.banks[class_index(destination_class)];
    const auto source_scheduled = schedule(source_class,
                                           issue_cycle,
                                           source.read_cycles);
    if (retention_failed(line, source, source_scheduled.start)) {
        ++stats_.expirations;
        return failure(EvidenceFailure::Expired, source_scheduled.start);
    }
    const auto destination_issue = checked_add(
        source_scheduled.completion,
        profile_.migration_setup_cycles,
        "migration setup overflow");
    const auto destination_scheduled = schedule(destination_class,
                                                destination_issue,
                                                destination.write_cycles);
    --banks_[class_index(source_class)].used_lines;
    ++banks_[class_index(destination_class)].used_lines;
    line.retention_class = destination_class;
    line.failed = false;
    line.last_write_cycle = destination_scheduled.completion;
    line.last_retention_check = destination_scheduled.completion;
    ++stats_.migrations;
    stats_.energy.migration_pj += source.read_energy_pj +
                                  destination.write_energy_pj +
                                  profile_.migration_setup_pj;
    account_control(true, true);
    return {true,
            EvidenceFailure::None,
            source_scheduled.start,
            destination_scheduled.completion};
}

std::optional<EvidenceLineStatus> EvidenceMemory::line_status(
    std::uint64_t byte_address) const {
    const auto found = lines_.find(logical_line(byte_address));
    if (found == lines_.end()) {
        return std::nullopt;
    }
    const auto& line = found->second;
    EvidenceLineStatus result;
    result.present = true;
    result.initialized = line.initialized;
    result.failed = line.failed;
    result.retention_class = line.retention_class;
    result.last_write_cycle = line.last_write_cycle;
    const auto retention =
        profile_.banks[class_index(line.retention_class)].retention_cycles;
    result.deadline_cycle =
        retention == kInfinite || line.last_write_cycle > kInfinite - retention
            ? kInfinite
            : line.last_write_cycle + retention;
    return result;
}

std::uint64_t EvidenceMemory::earliest_start_cycle(
    RetentionClass retention_class,
    std::uint64_t issue_cycle) const {
    const auto& ready = banks_[class_index(retention_class)].channel_ready;
    if (ready.empty()) {
        throw std::runtime_error("retention bank has no channels");
    }
    return std::max(issue_cycle,
                    *std::min_element(ready.begin(), ready.end()));
}

std::uint64_t EvidenceMemory::makespan_cycles() const {
    return makespan_cycles_;
}

std::uint64_t EvidenceMemory::bank_ready_cycle() const {
    std::uint64_t result = 0;
    for (const auto& bank : banks_) {
        for (const auto ready : bank.channel_ready) {
            result = std::max(result, ready);
        }
    }
    return result;
}

const EvidenceModelStatistics& EvidenceMemory::statistics() const {
    return stats_;
}

EvidenceEnergy EvidenceMemory::final_energy(
    std::uint64_t makespan_cycles) const {
    auto result = stats_.energy;
    double static_power_mw = 0.0;
    for (const auto power : profile_.static_power_mw) {
        static_power_mw += power;
    }
    result.static_pj = static_power_mw *
                       static_cast<double>(makespan_cycles) *
                       profile_.tick_ns;
    return result;
}

const char* to_string(EvidenceFailure failure) {
    switch (failure) {
        case EvidenceFailure::None:
            return "none";
        case EvidenceFailure::Capacity:
            return "capacity";
        case EvidenceFailure::Missing:
            return "missing";
        case EvidenceFailure::Uninitialized:
            return "uninitialized";
        case EvidenceFailure::Expired:
            return "expired";
    }
    return "unknown";
}

}  // namespace rtmem
