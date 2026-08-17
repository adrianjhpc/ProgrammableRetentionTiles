#include "retention_model.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace rtmem {
namespace {

constexpr std::uint64_t kInfiniteRetention =
    std::numeric_limits<std::uint64_t>::max();

BankConfig ephemeral_config() {
    return {"EPHEMERAL", 64, 128, 2, 2, 1.0, 2.0};
}

BankConfig epoch_config() {
    return {"EPOCH", 64, 4096, 3, 4, 1.4, 4.5};
}

BankConfig durable_config() {
    return {"DURABLE", 64, kInfiniteRetention, 4, 12, 2.0, 12.0};
}

}  // namespace

RetentionMemory::RetentionMemory(ErrorMode mode, std::uint64_t seed)
    : error_mode_(mode), rng_(seed) {
    banks_[0].config = ephemeral_config();
    banks_[1].config = epoch_config();
    banks_[2].config = durable_config();
    for (auto& bank : banks_) {
        bank.lines.resize(bank.config.capacity_lines);
    }
}

std::size_t RetentionMemory::class_index(RetentionClass retention_class) {
    return static_cast<std::size_t>(retention_class);
}

std::uint64_t RetentionMemory::logical_line(std::uint64_t byte_address) {
    return byte_address / kLineBytes;
}

const BankConfig& RetentionMemory::bank_config(
    RetentionClass retention_class) const {
    return banks_.at(class_index(retention_class)).config;
}

std::optional<std::size_t> RetentionMemory::allocate_slot(
    RetentionClass retention_class) {
    auto& lines = banks_.at(class_index(retention_class)).lines;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (!lines[index].valid) {
            return index;
        }
    }
    return std::nullopt;
}

void RetentionMemory::release(const Location& location) {
    banks_.at(class_index(location.retention_class)).lines.at(location.slot) =
        Line{};
}

bool RetentionMemory::retention_failed(Line& line,
                                       const BankConfig& config) {
    if (!line.valid || line.retention_failed ||
        config.retention_cycles == kInfiniteRetention) {
        return line.retention_failed;
    }

    const std::uint64_t age = now_ - line.last_write_cycle;
    if (error_mode_ == ErrorMode::Deterministic) {
        line.retention_failed = age >= config.retention_cycles;
        return line.retention_failed;
    }

    const std::uint64_t elapsed = now_ - line.last_retention_check;
    line.last_retention_check = now_;
    if (elapsed == 0) {
        return false;
    }

    const double probability =
        1.0 - std::exp(-static_cast<double>(elapsed) /
                       static_cast<double>(config.retention_cycles));
    line.retention_failed = uniform_(rng_) < probability;
    return line.retention_failed;
}

Result RetentionMemory::missing_result(const std::string& message,
                                       bool expired) {
    return {false, expired, 0, 0, message};
}

Result RetentionMemory::write(std::uint64_t byte_address,
                              RetentionClass requested_class,
                              std::uint64_t value) {
    const auto line_address = logical_line(byte_address);
    auto existing = directory_.find(line_address);
    std::optional<std::size_t> target_slot;

    if (existing != directory_.end() &&
        existing->second.retention_class == requested_class) {
        target_slot = existing->second.slot;
    } else {
        target_slot = allocate_slot(requested_class);
    }

    if (!target_slot) {
        ++stats_.capacity_failures;
        return missing_result("destination bank is full");
    }

    if (existing != directory_.end() &&
        existing->second.retention_class != requested_class) {
        release(existing->second);
    }

    auto& bank = banks_.at(class_index(requested_class));
    now_ += bank.config.write_cycles;
    stats_.energy_pj += bank.config.write_energy_pj;
    ++stats_.writes;

    auto& line = bank.lines.at(*target_slot);
    line.valid = true;
    line.retention_failed = false;
    line.logical_line = line_address;
    line.value = value;
    line.last_write_cycle = now_;
    line.last_retention_check = now_;
    directory_[line_address] = {requested_class, *target_slot};

    return {true, false, value, bank.config.write_cycles, "write complete"};
}

Result RetentionMemory::read(std::uint64_t byte_address) {
    ++stats_.reads;
    const auto line_address = logical_line(byte_address);
    const auto found = directory_.find(line_address);
    if (found == directory_.end()) {
        ++stats_.read_misses;
        return missing_result("line is not allocated");
    }

    const Location location = found->second;
    auto& bank = banks_.at(class_index(location.retention_class));
    auto& line = bank.lines.at(location.slot);
    if (retention_failed(line, bank.config)) {
        ++stats_.read_misses;
        ++stats_.expirations;
        directory_.erase(found);
        release(location);
        return missing_result("retention deadline exceeded", true);
    }

    now_ += bank.config.read_cycles;
    stats_.energy_pj += bank.config.read_energy_pj;
    ++stats_.read_hits;
    return {true, false, line.value, bank.config.read_cycles, "read hit"};
}

Result RetentionMemory::refresh(std::uint64_t byte_address) {
    const auto line_address = logical_line(byte_address);
    const auto found = directory_.find(line_address);
    if (found == directory_.end()) {
        return missing_result("cannot refresh an unallocated line");
    }

    const Location location = found->second;
    auto& bank = banks_.at(class_index(location.retention_class));
    auto& line = bank.lines.at(location.slot);
    if (retention_failed(line, bank.config)) {
        ++stats_.expirations;
        directory_.erase(found);
        release(location);
        return missing_result("line expired before refresh", true);
    }

    const auto latency = bank.config.read_cycles + bank.config.write_cycles;
    now_ += latency;
    stats_.energy_pj +=
        bank.config.read_energy_pj + bank.config.write_energy_pj;
    ++stats_.refreshes;
    line.last_write_cycle = now_;
    line.last_retention_check = now_;
    return {true, false, line.value, latency, "refresh complete"};
}

Result RetentionMemory::migrate(std::uint64_t byte_address,
                                RetentionClass destination_class) {
    const auto line_address = logical_line(byte_address);
    const auto found = directory_.find(line_address);
    if (found == directory_.end()) {
        return missing_result("cannot migrate an unallocated line");
    }

    const Location source_location = found->second;
    if (source_location.retention_class == destination_class) {
        return refresh(byte_address);
    }

    auto& source_bank = banks_.at(class_index(source_location.retention_class));
    auto& source_line = source_bank.lines.at(source_location.slot);
    if (retention_failed(source_line, source_bank.config)) {
        ++stats_.expirations;
        directory_.erase(found);
        release(source_location);
        return missing_result("line expired before migration", true);
    }

    const auto target_slot = allocate_slot(destination_class);
    if (!target_slot) {
        ++stats_.capacity_failures;
        return missing_result("destination bank is full");
    }

    const auto value = source_line.value;
    auto& destination_bank = banks_.at(class_index(destination_class));
    const auto latency =
        source_bank.config.read_cycles + destination_bank.config.write_cycles;
    now_ += latency;
    stats_.energy_pj += source_bank.config.read_energy_pj +
                        destination_bank.config.write_energy_pj;
    ++stats_.migrations;

    auto& destination_line = destination_bank.lines.at(*target_slot);
    destination_line.valid = true;
    destination_line.retention_failed = false;
    destination_line.logical_line = line_address;
    destination_line.value = value;
    destination_line.last_write_cycle = now_;
    destination_line.last_retention_check = now_;

    release(source_location);
    directory_[line_address] = {destination_class, *target_slot};
    return {true, false, value, latency, "migration complete"};
}

Result RetentionMemory::invalidate(std::uint64_t byte_address) {
    const auto line_address = logical_line(byte_address);
    const auto found = directory_.find(line_address);
    if (found == directory_.end()) {
        return missing_result("line is already invalid");
    }
    release(found->second);
    directory_.erase(found);
    ++stats_.invalidations;
    return {true, false, 0, 0, "line invalidated"};
}

void RetentionMemory::advance(std::uint64_t cycles) {
    now_ += cycles;
}

void RetentionMemory::print_statistics(std::ostream& output) const {
    output << "cycles=" << now_ << '\n'
           << "reads=" << stats_.reads << '\n'
           << "read_hits=" << stats_.read_hits << '\n'
           << "read_misses=" << stats_.read_misses << '\n'
           << "writes=" << stats_.writes << '\n'
           << "refreshes=" << stats_.refreshes << '\n'
           << "migrations=" << stats_.migrations << '\n'
           << "invalidations=" << stats_.invalidations << '\n'
           << "expirations=" << stats_.expirations << '\n'
           << "capacity_failures=" << stats_.capacity_failures << '\n'
           << std::fixed << std::setprecision(2)
           << "energy_pj=" << stats_.energy_pj << '\n';
}

std::string to_string(RetentionClass retention_class) {
    switch (retention_class) {
        case RetentionClass::Ephemeral:
            return "EPHEMERAL";
        case RetentionClass::Epoch:
            return "EPOCH";
        case RetentionClass::Durable:
            return "DURABLE";
    }
    throw std::invalid_argument("unknown retention class");
}

RetentionClass parse_retention_class(const std::string& text) {
    if (text == "EPHEMERAL" || text == "FAST") {
        return RetentionClass::Ephemeral;
    }
    if (text == "EPOCH" || text == "MEDIUM") {
        return RetentionClass::Epoch;
    }
    if (text == "DURABLE" || text == "PERSISTENT") {
        return RetentionClass::Durable;
    }
    throw std::invalid_argument("unknown retention class: " + text);
}

}  // namespace rtmem

