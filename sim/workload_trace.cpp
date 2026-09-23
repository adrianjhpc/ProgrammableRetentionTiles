#include "workload_trace.hpp"

#include "evidence_model.hpp"

// RTTRACE 2 policy-replay layer. This file parses policy-independent workload
// events, schedules their streams, applies placement/maintenance policies, and
// reports attributable evidence. Capacity, line validity, bank queues, and
// energy accounting live in EvidenceMemory (evidence_model.cpp).

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rtmem {
namespace {

constexpr std::uint64_t kInfinite =
    std::numeric_limits<std::uint64_t>::max();

enum class EventKind {
    Allocate,
    Free,
    Hint,
    Access,
    Compute,
    Phase,
    Barrier,
};

struct Event {
    EventKind kind = EventKind::Phase;
    std::uint64_t sequence = 0;
    std::uint64_t tick = 0;
    std::uint32_t thread = 0;
    std::uint32_t stream = 0;
    std::uint64_t buffer = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
    std::uint64_t offset = 0;
    std::uint64_t cycles = 0;
    std::uint64_t barrier = 0;
    RetentionClass hint = RetentionClass::Epoch;
    bool write = false;
    std::string name;
};

struct Allocation {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint64_t lines = 0;
    RetentionClass retention_class = RetentionClass::Epoch;
    std::string name;
    bool resident = false;
};

struct ClassStatistics {
    std::uint64_t allocations = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t live_bytes = 0;
    std::uint64_t peak_live_bytes = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
};

struct StructureStatistics {
    std::uint64_t allocations = 0;
    std::uint64_t allocated_bytes = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint8_t class_mask = 0;
};

struct FailureStatistics {
    std::uint64_t allocation_failures = 0;
    std::uint64_t capacity_overflow_bytes = 0;
    std::uint64_t expired_accesses = 0;
    std::uint64_t uninitialized_reads = 0;
    std::uint64_t missing_accesses = 0;
    std::uint64_t migration_failures = 0;
    std::uint64_t maintenance_failures = 0;
    std::uint64_t rejected_access_events = 0;

    std::uint64_t total() const {
        return allocation_failures + expired_accesses + uninitialized_reads +
               missing_accesses + migration_failures + maintenance_failures +
               rejected_access_events;
    }
};

struct ReplayStatistics {
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t access_events = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t compute_cycles = 0;
    std::uint64_t phases = 0;
    std::uint64_t barriers = 0;
    std::uint64_t invalidated_lines = 0;
    std::uint64_t unsafe_accesses = 0;
    FailureStatistics failures;
    std::array<ClassStatistics, 3> classes{};
    std::map<std::string, StructureStatistics> structures;
};

struct ReplayReport {
    ReplayStatistics replay;
    EvidenceModelStatistics model;
    EvidenceEnergy energy;
    std::uint64_t makespan_cycles = 0;
    std::string trace_fingerprint;
};

std::size_t class_index(RetentionClass retention_class) {
    return static_cast<std::size_t>(retention_class);
}

std::uint8_t class_bit(RetentionClass retention_class) {
    return static_cast<std::uint8_t>(1U << class_index(retention_class));
}

std::string class_mask_name(std::uint8_t mask) {
    std::string result;
    for (const auto retention_class : {RetentionClass::Ephemeral,
                                       RetentionClass::Epoch,
                                       RetentionClass::Durable}) {
        if ((mask & class_bit(retention_class)) == 0) {
            continue;
        }
        if (!result.empty()) {
            result += '|';
        }
        result += to_string(retention_class);
    }
    return result.empty() ? "NONE" : result;
}

void add_live_bytes(ReplayStatistics& replay,
                    RetentionClass retention_class,
                    std::uint64_t bytes) {
    auto& statistics = replay.classes.at(class_index(retention_class));
    if (statistics.live_bytes > kInfinite - bytes) {
        throw std::runtime_error("retention-class live-byte overflow");
    }
    statistics.live_bytes += bytes;
    statistics.peak_live_bytes =
        std::max(statistics.peak_live_bytes, statistics.live_bytes);
}

void remove_live_bytes(ReplayStatistics& replay,
                       RetentionClass retention_class,
                       std::uint64_t bytes) {
    auto& statistics = replay.classes.at(class_index(retention_class));
    if (statistics.live_bytes < bytes) {
        throw std::runtime_error("retention-class live-byte underflow");
    }
    statistics.live_bytes -= bytes;
}

std::vector<std::string> tokens(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> result;
    std::string token;
    while (input >> token) {
        if (!token.empty() && token.front() == '#') {
            break;
        }
        result.push_back(token);
    }
    return result;
}

std::uint64_t number(const std::string& text, const char* field) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("invalid ") + field);
    }
    return value;
}

std::uint32_t number32(const std::string& text, const char* field) {
    const auto value = number(text, field);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(field) + " exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(value);
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open workload trace: " + path);
    }
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::string fnv1a64(const std::string& contents) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : contents) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0')
           << hash;
    return output.str();
}

std::vector<Event> load_events(const std::string& path) {
    const auto contents = read_file(path);
    std::istringstream input(contents);
    std::vector<Event> events;
    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;
    std::uint64_t previous_sequence = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const auto fields = tokens(line);
        if (fields.empty()) {
            continue;
        }
        try {
            if (!header_seen) {
                if (fields.size() != 2 || fields[0] != "RTTRACE" ||
                    fields[1] != "2") {
                    throw std::runtime_error("expected RTTRACE 2 header");
                }
                header_seen = true;
                continue;
            }

            Event event;
            if (fields.size() < 4) {
                throw std::runtime_error("trace event is too short");
            }
            event.sequence = number(fields[1], "sequence");
            event.tick = number(fields[2], "tick");
            event.thread = number32(fields[3], "thread");
            if ((events.empty() && event.sequence != 0) ||
                (!events.empty() &&
                 event.sequence != previous_sequence + 1)) {
                throw std::runtime_error(
                    "event sequence must start at zero and be contiguous");
            }
            previous_sequence = event.sequence;

            if (fields[0] == "ALLOC" && fields.size() == 9) {
                event.kind = EventKind::Allocate;
                event.buffer = number(fields[4], "buffer");
                event.size = number(fields[5], "size");
                event.alignment = number(fields[6], "alignment");
                event.hint = parse_retention_class(fields[7]);
                event.name = fields[8];
                if (event.size == 0) {
                    throw std::runtime_error("zero-sized allocation");
                }
                if (event.alignment == 0 ||
                    (event.alignment & (event.alignment - 1)) != 0) {
                    throw std::runtime_error(
                        "allocation alignment must be a power of two");
                }
            } else if (fields[0] == "FREE" && fields.size() == 5) {
                event.kind = EventKind::Free;
                event.buffer = number(fields[4], "buffer");
            } else if (fields[0] == "HINT" && fields.size() == 6) {
                event.kind = EventKind::Hint;
                event.buffer = number(fields[4], "buffer");
                event.hint = parse_retention_class(fields[5]);
            } else if (fields[0] == "ACCESS" && fields.size() == 9) {
                event.kind = EventKind::Access;
                event.stream = number32(fields[4], "stream");
                if (fields[5] != "R" && fields[5] != "W") {
                    throw std::runtime_error("access must be R or W");
                }
                event.write = fields[5] == "W";
                event.buffer = number(fields[6], "buffer");
                event.offset = number(fields[7], "offset");
                event.size = number(fields[8], "size");
                if (event.size == 0) {
                    throw std::runtime_error("zero-sized access");
                }
            } else if (fields[0] == "COMPUTE" && fields.size() == 6) {
                event.kind = EventKind::Compute;
                event.stream = number32(fields[4], "stream");
                event.cycles = number(fields[5], "cycles");
            } else if (fields[0] == "PHASE" && fields.size() == 5) {
                event.kind = EventKind::Phase;
                event.name = fields[4];
            } else if (fields[0] == "BARRIER" && fields.size() == 6) {
                event.kind = EventKind::Barrier;
                event.stream = number32(fields[4], "stream");
                event.barrier = number(fields[5], "barrier");
            } else {
                throw std::runtime_error("unknown event or wrong field count");
            }
            events.push_back(std::move(event));
        } catch (const std::exception& error) {
            std::ostringstream message;
            message << path << ':' << line_number << ": " << error.what();
            throw std::runtime_error(message.str());
        }
    }
    if (!header_seen) {
        throw std::runtime_error("empty workload trace");
    }
    return events;
}

std::pair<std::uint64_t, std::uint64_t> line_range(const Event& event) {
    if (event.offset > kInfinite - (event.size - 1)) {
        throw std::runtime_error("access range overflow");
    }
    return {event.offset / kLineBytes,
            (event.offset + event.size - 1) / kLineBytes};
}

std::uint64_t aligned_lines(std::uint64_t size) {
    if (size > kInfinite - (kLineBytes - 1)) {
        throw std::runtime_error("allocation size overflow");
    }
    return (size + kLineBytes - 1) / kLineBytes;
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::runtime_error("invalid alignment");
    }
    const auto mask = alignment - 1;
    if (value > kInfinite - mask) {
        throw std::runtime_error("logical address alignment overflow");
    }
    return (value + mask) & ~mask;
}

std::uint64_t checked_add(std::uint64_t left,
                          std::uint64_t right,
                          const char* message) {
    if (left > kInfinite - right) {
        throw std::runtime_error(message);
    }
    return left + right;
}

std::uint64_t stream_key(std::uint32_t thread, std::uint32_t stream) {
    return (static_cast<std::uint64_t>(thread) << 32U) | stream;
}

std::unordered_map<std::uint64_t, std::uint64_t> oracle_ages(
    const std::vector<Event>& events,
    const DeviceProfile& profile) {
    struct AgeState {
        std::uint64_t size = 0;
        std::vector<std::uint64_t> last_write;
        std::vector<bool> initialized;
    };
    std::unordered_map<std::uint64_t, AgeState> states;
    std::unordered_map<std::uint64_t, std::uint64_t> result;
    std::uint64_t cycle = 0;
    const auto maximum_read_cycles = std::max(
        {profile.banks[0].read_cycles,
         profile.banks[1].read_cycles,
         profile.banks[2].read_cycles});
    const auto maximum_write_cycles = std::max(
        {profile.banks[0].write_cycles,
         profile.banks[1].write_cycles,
         profile.banks[2].write_cycles});

    for (const auto& event : events) {
        if (event.kind == EventKind::Allocate) {
            AgeState state;
            state.size = event.size;
            state.last_write.resize(aligned_lines(event.size));
            state.initialized.resize(aligned_lines(event.size));
            states[event.buffer] = std::move(state);
        } else if (event.kind == EventKind::Access) {
            auto found = states.find(event.buffer);
            if (found == states.end() || event.offset > found->second.size ||
                event.size > found->second.size - event.offset) {
                throw std::runtime_error("access references invalid allocation");
            }
            const auto range = line_range(event);
            for (auto line = range.first; line <= range.second; ++line) {
                if (event.write) {
                    cycle = checked_add(cycle,
                                        maximum_write_cycles,
                                        "oracle cycle overflow");
                    found->second.last_write.at(line) = cycle;
                    found->second.initialized.at(line) = true;
                } else {
                    if (found->second.initialized.at(line)) {
                        result[event.buffer] = std::max(
                            result[event.buffer],
                            cycle - found->second.last_write.at(line));
                    }
                    cycle = checked_add(cycle,
                                        maximum_read_cycles,
                                        "oracle cycle overflow");
                }
            }
        } else if (event.kind == EventKind::Compute) {
            cycle = checked_add(cycle, event.cycles, "oracle cycle overflow");
        } else if (event.kind == EventKind::Free) {
            states.erase(event.buffer);
        }
    }
    return result;
}

RetentionClass oracle_class(std::uint64_t age,
                            const DeviceProfile& profile) {
    if (age < profile.banks[class_index(RetentionClass::Ephemeral)]
                  .retention_cycles) {
        return RetentionClass::Ephemeral;
    }
    if (age < profile.banks[class_index(RetentionClass::Epoch)]
                  .retention_cycles) {
        return RetentionClass::Epoch;
    }
    return RetentionClass::Durable;
}

RetentionClass selected_class(
    ReplayPolicy policy,
    const Event& allocation,
    const std::unordered_map<std::uint64_t, std::uint64_t>& ages,
    const DeviceProfile& profile) {
    switch (policy) {
        case ReplayPolicy::Hint:
        case ReplayPolicy::Refresh:
        case ReplayPolicy::Adaptive:
            return allocation.hint;
        case ReplayPolicy::Ephemeral:
            return RetentionClass::Ephemeral;
        case ReplayPolicy::Epoch:
            return RetentionClass::Epoch;
        case ReplayPolicy::Durable:
            return RetentionClass::Durable;
        case ReplayPolicy::Oracle: {
            const auto found = ages.find(allocation.buffer);
            return oracle_class(found == ages.end() ? 0 : found->second,
                                profile);
        }
    }
    throw std::runtime_error("invalid replay policy");
}

RetentionClass next_class(RetentionClass selected) {
    if (selected == RetentionClass::Ephemeral) {
        return RetentionClass::Epoch;
    }
    return RetentionClass::Durable;
}

void record_failure(ReplayStatistics& replay,
                    EvidenceFailure failure,
                    bool maintenance = false,
                    bool migration = false) {
    ++replay.unsafe_accesses;
    if (maintenance) {
        ++replay.failures.maintenance_failures;
    }
    if (migration) {
        ++replay.failures.migration_failures;
    }
    switch (failure) {
        case EvidenceFailure::None:
            break;
        case EvidenceFailure::Capacity:
            break;
        case EvidenceFailure::Missing:
            ++replay.failures.missing_accesses;
            break;
        case EvidenceFailure::Uninitialized:
            ++replay.failures.uninitialized_reads;
            break;
        case EvidenceFailure::Expired:
            ++replay.failures.expired_accesses;
            break;
    }
}

bool reserve_with_policy(EvidenceMemory& memory,
                         ReplayPolicy policy,
                         RetentionClass requested,
                         std::uint64_t base,
                         std::uint64_t lines,
                         RetentionClass& selected) {
    selected = requested;
    if (memory.reserve_range(base, lines, selected)) {
        return true;
    }
    if (policy != ReplayPolicy::Adaptive) {
        return false;
    }
    while (selected != RetentionClass::Durable) {
        selected = next_class(selected);
        if (memory.reserve_range(base, lines, selected)) {
            return true;
        }
    }
    return false;
}

bool migrate_allocation(EvidenceMemory& memory,
                        Allocation& allocation,
                        RetentionClass destination,
                        std::uint64_t issue_cycle,
                        std::uint64_t& completion,
                        ReplayStatistics& replay,
                        bool maintenance) {
    if (allocation.retention_class == destination) {
        completion = issue_cycle;
        return true;
    }
    if (!memory.can_fit(destination, allocation.lines)) {
        if (!maintenance) {
            ++replay.unsafe_accesses;
            ++replay.failures.migration_failures;
        }
        completion = issue_cycle;
        return false;
    }
    completion = issue_cycle;
    for (std::uint64_t line = 0; line < allocation.lines; ++line) {
        const auto operation = memory.migrate(
            allocation.base + line * kLineBytes,
            destination,
            completion);
        if (!operation.ok) {
            record_failure(replay,
                           operation.failure,
                           maintenance,
                           true);
            return false;
        }
        completion = std::max(completion, operation.completion_cycle);
    }
    remove_live_bytes(replay,
                      allocation.retention_class,
                      allocation.size);
    add_live_bytes(replay, destination, allocation.size);
    allocation.retention_class = destination;
    replay.structures[allocation.name].class_mask |= class_bit(destination);
    return true;
}

bool ensure_retained(EvidenceMemory& memory,
                     Allocation& allocation,
                     std::uint64_t byte_address,
                     std::uint64_t needed_cycle,
                     ReplayPolicy policy,
                     std::uint64_t guard_cycles,
                     std::uint64_t& completion,
                     ReplayStatistics& replay) {
    completion = needed_cycle;
    if (policy != ReplayPolicy::Refresh &&
        policy != ReplayPolicy::Adaptive) {
        return true;
    }

    for (std::uint64_t attempts = 0; attempts < 100000; ++attempts) {
        const auto status = memory.line_status(byte_address);
        if (!status || !status->initialized ||
            status->deadline_cycle == kInfinite) {
            return true;
        }
        const auto observation_cycle = memory.earliest_start_cycle(
            status->retention_class,
            completion);
        if (observation_cycle < status->deadline_cycle) {
            return true;
        }

        const auto& bank = status->retention_class;
        if (policy == ReplayPolicy::Adaptive &&
            bank != RetentionClass::Durable) {
            const auto destination = next_class(bank);
            const auto start = status->deadline_cycle > guard_cycles
                                   ? status->deadline_cycle - guard_cycles
                                   : status->last_write_cycle;
            std::uint64_t migrated = start;
            if (migrate_allocation(memory,
                                   allocation,
                                   destination,
                                   start,
                                   migrated,
                                   replay,
                                   true)) {
                completion = std::max(completion, migrated);
                continue;
            }
            // Capacity pressure falls back to refresh in the current class.
        }

        const auto start = status->deadline_cycle > guard_cycles
                               ? status->deadline_cycle - guard_cycles
                               : status->last_write_cycle;
        const auto refreshed = memory.refresh(byte_address, start);
        if (!refreshed.ok ||
            refreshed.completion_cycle > status->deadline_cycle) {
            record_failure(replay,
                           refreshed.ok ? EvidenceFailure::Expired
                                        : refreshed.failure,
                           true,
                           false);
            return false;
        }
        completion = std::max(completion, refreshed.completion_cycle);
    }
    throw std::runtime_error("maintenance iteration limit exceeded");
}

std::uint64_t all_streams_ready(
    const std::unordered_map<std::uint64_t, std::uint64_t>& ready) {
    std::uint64_t result = 0;
    for (const auto& entry : ready) {
        result = std::max(result, entry.second);
    }
    return result;
}

std::string json_escape(const std::string& text) {
    std::string result;
    for (const unsigned char byte : text) {
        switch (byte) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (byte < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << static_cast<int>(byte);
                    result += escaped.str();
                } else {
                    result.push_back(static_cast<char>(byte));
                }
        }
    }
    return result;
}

void write_json(const std::string& path,
                const std::string& trace_path,
                ReplayPolicy policy,
                const DeviceProfile& profile,
                const std::vector<Event>& events,
                const ReplayReport& report) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open JSON report: " + path);
    }
    const auto safe = report.replay.unsafe_accesses == 0;
    output << "{\n"
           << "  \"schema\": \"RTMEM_EVIDENCE_REPORT_1\",\n"
           << "  \"trace\": \"" << json_escape(trace_path) << "\",\n"
           << "  \"trace_fingerprint\": \""
           << report.trace_fingerprint << "\",\n"
           << "  \"profile_id\": \"" << json_escape(profile.profile_id)
           << "\",\n"
           << "  \"profile_source\": \"" << json_escape(profile.source)
           << "\",\n"
           << "  \"profile_fingerprint\": \""
           << profile.fingerprint << "\",\n"
           << "  \"policy\": \"" << to_string(policy) << "\",\n"
           << "  \"status\": \"" << (safe ? "SAFE" : "UNSAFE")
           << "\",\n"
           << "  \"performance_metrics_valid\": "
           << (safe ? "true" : "false") << ",\n"
           << "  \"cycles\": " << report.makespan_cycles << ",\n"
           << std::fixed << std::setprecision(6)
           << "  \"modeled_energy_pj\": " << report.energy.total() << ",\n"
           << "  \"workload\": {\n"
           << "    \"trace_events\": " << events.size() << ",\n"
           << "    \"allocations\": " << report.replay.allocations << ",\n"
           << "    \"frees\": " << report.replay.frees << ",\n"
           << "    \"access_events\": " << report.replay.access_events
           << ",\n"
           << "    \"read_bytes\": " << report.replay.read_bytes << ",\n"
           << "    \"write_bytes\": " << report.replay.write_bytes << ",\n"
           << "    \"compute_cycles\": " << report.replay.compute_cycles
           << ",\n"
           << "    \"phases\": " << report.replay.phases << ",\n"
           << "    \"barriers\": " << report.replay.barriers << "\n"
           << "  },\n"
           << "  \"model\": {\n"
           << "    \"read_attempts\": " << report.model.read_attempts
           << ",\n"
           << "    \"useful_reads\": " << report.model.useful_reads
           << ",\n"
           << "    \"write_attempts\": " << report.model.write_attempts
           << ",\n"
           << "    \"useful_writes\": " << report.model.useful_writes
           << ",\n"
           << "    \"refreshes\": " << report.model.refreshes << ",\n"
           << "    \"migrations\": " << report.model.migrations << ",\n"
           << "    \"queue_delay_cycles\": "
           << report.model.queue_delay_cycles << "\n"
           << "  },\n"
           << "  \"energy\": {\n"
           << "    \"useful_read_pj\": "
           << report.energy.useful_read_pj << ",\n"
           << "    \"useful_write_pj\": "
           << report.energy.useful_write_pj << ",\n"
           << "    \"refresh_pj\": " << report.energy.refresh_pj << ",\n"
           << "    \"migration_pj\": " << report.energy.migration_pj
           << ",\n"
           << "    \"controller_pj\": " << report.energy.controller_pj
           << ",\n"
           << "    \"metadata_pj\": " << report.energy.metadata_pj
           << ",\n"
           << "    \"ecc_pj\": " << report.energy.ecc_pj << ",\n"
           << "    \"static_pj\": " << report.energy.static_pj << "\n"
           << "  },\n"
           << "  \"failures\": {\n"
           << "    \"unsafe_accesses\": "
           << report.replay.unsafe_accesses << ",\n"
           << "    \"allocation_failures\": "
           << report.replay.failures.allocation_failures << ",\n"
           << "    \"capacity_overflow_bytes\": "
           << report.replay.failures.capacity_overflow_bytes << ",\n"
           << "    \"expired_accesses\": "
           << report.replay.failures.expired_accesses << ",\n"
           << "    \"uninitialized_reads\": "
           << report.replay.failures.uninitialized_reads << ",\n"
           << "    \"missing_accesses\": "
           << report.replay.failures.missing_accesses << ",\n"
           << "    \"migration_failures\": "
           << report.replay.failures.migration_failures << ",\n"
           << "    \"maintenance_failures\": "
           << report.replay.failures.maintenance_failures << ",\n"
           << "    \"rejected_access_events\": "
           << report.replay.failures.rejected_access_events << "\n"
           << "  },\n"
           << "  \"classes\": {\n";
    for (std::size_t index = 0; index < 3; ++index) {
        const auto retention_class =
            static_cast<RetentionClass>(index);
        const auto& statistics = report.replay.classes[index];
        output << "    \"" << to_string(retention_class) << "\": {"
               << "\"allocations\": " << statistics.allocations
               << ", \"allocated_bytes\": " << statistics.allocated_bytes
               << ", \"peak_live_bytes\": " << statistics.peak_live_bytes
               << ", \"read_bytes\": " << statistics.read_bytes
               << ", \"write_bytes\": " << statistics.write_bytes << "}"
               << (index == 2 ? "\n" : ",\n");
    }
    output << "  },\n"
           << "  \"structures\": {\n";
    std::size_t structure_index = 0;
    for (const auto& entry : report.replay.structures) {
        const auto& statistics = entry.second;
        output << "    \"" << json_escape(entry.first) << "\": {"
               << "\"allocations\": " << statistics.allocations
               << ", \"allocated_bytes\": " << statistics.allocated_bytes
               << ", \"read_bytes\": " << statistics.read_bytes
               << ", \"write_bytes\": " << statistics.write_bytes
               << ", \"placement\": \""
               << class_mask_name(statistics.class_mask) << "\"}"
               << (++structure_index == report.replay.structures.size()
                       ? "\n"
                       : ",\n");
    }
    output << "  }\n"
           << "}\n";
    if (!output) {
        throw std::runtime_error("cannot write JSON report: " + path);
    }
}

ReplayReport replay_events(const std::string& trace_path,
                           const std::vector<Event>& events,
                           ReplayPolicy policy,
                           ErrorMode mode,
                           std::uint64_t seed,
                           const DeviceProfile& profile,
                           const ReplayOptions& options) {
    EvidenceMemory memory(profile, mode, seed);
    const auto ages = oracle_ages(events, profile);
    std::unordered_map<std::uint64_t, Allocation> allocations;
    std::unordered_map<std::uint64_t, std::uint64_t> stream_ready;
    ReplayStatistics replay;
    std::uint64_t next_base = 0;

    for (const auto& event : events) {
        const auto key = stream_key(event.thread, event.stream);
        auto& ready = stream_ready[key];
        ready = std::max(ready, event.tick);

        if (event.kind == EventKind::Allocate) {
            if (allocations.count(event.buffer) != 0) {
                throw std::runtime_error("duplicate live buffer id");
            }
            const auto lines = aligned_lines(event.size);
            const auto allocation_alignment =
                std::max<std::uint64_t>(event.alignment, kLineBytes);
            next_base = align_up(next_base, allocation_alignment);
            if (lines > kInfinite / kLineBytes ||
                next_base > kInfinite - lines * kLineBytes) {
                throw std::runtime_error("logical address overflow");
            }
            Allocation allocation;
            allocation.base = next_base;
            allocation.size = event.size;
            allocation.lines = lines;
            allocation.retention_class =
                selected_class(policy, event, ages, profile);
            allocation.name = event.name;
            next_base += lines * kLineBytes;

            RetentionClass actual = allocation.retention_class;
            allocation.resident = reserve_with_policy(memory,
                                                      policy,
                                                      allocation.retention_class,
                                                      allocation.base,
                                                      allocation.lines,
                                                      actual);
            allocation.retention_class = actual;
            if (!allocation.resident) {
                ++replay.unsafe_accesses;
                ++replay.failures.allocation_failures;
                replay.failures.capacity_overflow_bytes = checked_add(
                    replay.failures.capacity_overflow_bytes,
                    allocation.size,
                    "capacity-overflow counter overflow");
            } else {
                auto& class_statistics = replay.classes.at(
                    class_index(allocation.retention_class));
                ++class_statistics.allocations;
                class_statistics.allocated_bytes += allocation.size;
                add_live_bytes(replay,
                               allocation.retention_class,
                               allocation.size);
            }
            auto& structure = replay.structures[allocation.name];
            ++structure.allocations;
            structure.allocated_bytes += allocation.size;
            if (allocation.resident) {
                structure.class_mask |= class_bit(allocation.retention_class);
            }
            allocations.emplace(event.buffer, allocation);
            ++replay.allocations;
        } else if (event.kind == EventKind::Free) {
            const auto found = allocations.find(event.buffer);
            if (found == allocations.end()) {
                throw std::runtime_error("free references unknown buffer");
            }
            if (found->second.resident) {
                ready = std::max({ready,
                                  all_streams_ready(stream_ready),
                                  memory.bank_ready_cycle()});
                for (auto& entry : stream_ready) {
                    entry.second = ready;
                }
                memory.release_range(found->second.base, found->second.lines);
                remove_live_bytes(replay,
                                  found->second.retention_class,
                                  found->second.size);
                replay.invalidated_lines += found->second.lines;
            }
            allocations.erase(found);
            ++replay.frees;
        } else if (event.kind == EventKind::Hint) {
            const auto found = allocations.find(event.buffer);
            if (found == allocations.end()) {
                throw std::runtime_error("hint references unknown buffer");
            }
            if ((policy == ReplayPolicy::Hint ||
                 policy == ReplayPolicy::Refresh ||
                 policy == ReplayPolicy::Adaptive) &&
                found->second.resident &&
                found->second.retention_class != event.hint) {
                std::uint64_t completed = ready;
                if (migrate_allocation(memory,
                                       found->second,
                                       event.hint,
                                       ready,
                                       completed,
                                       replay,
                                       false)) {
                    ready = std::max(ready, completed);
                }
            }
        } else if (event.kind == EventKind::Access) {
            const auto found = allocations.find(event.buffer);
            if (found == allocations.end() ||
                event.offset > found->second.size ||
                event.size > found->second.size - event.offset) {
                throw std::runtime_error(
                    "access references unknown buffer or exceeds bounds");
            }
            ++replay.access_events;
            if (!found->second.resident) {
                ++replay.unsafe_accesses;
                ++replay.failures.rejected_access_events;
                continue;
            }

            const auto range = line_range(event);
            bool access_ok = true;
            for (auto line = range.first; line <= range.second; ++line) {
                const auto address =
                    found->second.base + line * kLineBytes;
                if (!event.write) {
                    std::uint64_t maintained = ready;
                    if (!ensure_retained(memory,
                                         found->second,
                                         address,
                                         ready,
                                         policy,
                                         options.maintenance_guard_cycles,
                                         maintained,
                                         replay)) {
                        access_ok = false;
                        break;
                    }
                    ready = std::max(ready, maintained);
                }
                const auto operation =
                    event.write
                        ? memory.write(address,
                                       found->second.retention_class,
                                       ready)
                        : memory.read(address, ready);
                if (!operation.ok) {
                    record_failure(replay, operation.failure);
                    access_ok = false;
                    break;
                }
                ready = std::max(ready, operation.completion_cycle);
            }
            if (!access_ok) {
                continue;
            }

            auto& class_statistics = replay.classes.at(
                class_index(found->second.retention_class));
            auto& structure = replay.structures[found->second.name];
            structure.class_mask |= class_bit(found->second.retention_class);
            if (event.write) {
                replay.write_bytes += event.size;
                class_statistics.write_bytes += event.size;
                structure.write_bytes += event.size;
            } else {
                replay.read_bytes += event.size;
                class_statistics.read_bytes += event.size;
                structure.read_bytes += event.size;
            }
        } else if (event.kind == EventKind::Compute) {
            ready = checked_add(ready,
                                event.cycles,
                                "stream compute cycle overflow");
            replay.compute_cycles = checked_add(replay.compute_cycles,
                                                event.cycles,
                                                "compute counter overflow");
        } else if (event.kind == EventKind::Phase) {
            ++replay.phases;
        } else if (event.kind == EventKind::Barrier) {
            const auto fence = std::max({ready,
                                         all_streams_ready(stream_ready),
                                         memory.bank_ready_cycle(),
                                         event.tick});
            for (auto& entry : stream_ready) {
                entry.second = fence;
            }
            ready = fence;
            ++replay.barriers;
        }
    }

    ReplayReport report;
    report.replay = std::move(replay);
    report.model = memory.statistics();
    report.makespan_cycles = std::max(all_streams_ready(stream_ready),
                                      memory.makespan_cycles());
    report.energy = memory.final_energy(report.makespan_cycles);
    report.trace_fingerprint = fnv1a64(read_file(trace_path));
    return report;
}

void print_report(const std::string& path,
                  ReplayPolicy policy,
                  const DeviceProfile& profile,
                  const std::vector<Event>& events,
                  const ReplayReport& report,
                  std::ostream& output) {
    const auto& replay = report.replay;
    const auto& model = report.model;
    const auto safe = replay.unsafe_accesses == 0;
    output << "workload_trace=" << path << '\n'
           << "trace_fingerprint=" << report.trace_fingerprint << '\n'
           << "profile_id=" << profile.profile_id << '\n'
           << "profile_source=" << profile.source << '\n'
           << "profile_fingerprint=" << profile.fingerprint << '\n'
           << "policy=" << to_string(policy) << '\n'
           << "timing_model=event_driven_bank_queued\n"
           << "energy_scope=modeled_memory_and_configured_overheads\n"
           << "trace_events=" << events.size() << '\n'
           << "allocations=" << replay.allocations << '\n'
           << "frees=" << replay.frees << '\n'
           << "access_events=" << replay.access_events << '\n'
           << "read_bytes=" << replay.read_bytes << '\n'
           << "write_bytes=" << replay.write_bytes << '\n'
           << "compute_cycles=" << replay.compute_cycles << '\n'
           << "phases=" << replay.phases << '\n'
           << "barriers=" << replay.barriers << '\n'
           << "unsafe_accesses=" << replay.unsafe_accesses << '\n'
           << "failure.allocation_failures="
           << replay.failures.allocation_failures << '\n'
           << "failure.capacity_overflow_bytes="
           << replay.failures.capacity_overflow_bytes << '\n'
           << "failure.expired_accesses="
           << replay.failures.expired_accesses << '\n'
           << "failure.uninitialized_reads="
           << replay.failures.uninitialized_reads << '\n'
           << "failure.missing_accesses="
           << replay.failures.missing_accesses << '\n'
           << "failure.migration_failures="
           << replay.failures.migration_failures << '\n'
           << "failure.maintenance_failures="
           << replay.failures.maintenance_failures << '\n'
           << "failure.rejected_access_events="
           << replay.failures.rejected_access_events << '\n'
           << "status=" << (safe ? "SAFE" : "UNSAFE") << '\n'
           << "performance_metrics_valid=" << (safe ? "true" : "false")
           << '\n';

    for (const auto retention_class : {RetentionClass::Ephemeral,
                                       RetentionClass::Epoch,
                                       RetentionClass::Durable}) {
        const auto& statistics =
            replay.classes.at(class_index(retention_class));
        const auto prefix = "class." + to_string(retention_class) + '.';
        output << prefix << "allocations=" << statistics.allocations << '\n'
               << prefix << "allocated_bytes="
               << statistics.allocated_bytes << '\n'
               << prefix << "peak_live_bytes="
               << statistics.peak_live_bytes << '\n'
               << prefix << "read_bytes=" << statistics.read_bytes << '\n'
               << prefix << "write_bytes=" << statistics.write_bytes << '\n';
    }
    for (const auto& entry : replay.structures) {
        const auto prefix = "structure." + entry.first + '.';
        const auto& statistics = entry.second;
        output << prefix << "allocations=" << statistics.allocations << '\n'
               << prefix << "allocated_bytes="
               << statistics.allocated_bytes << '\n'
               << prefix << "read_bytes=" << statistics.read_bytes << '\n'
               << prefix << "write_bytes=" << statistics.write_bytes << '\n'
               << prefix << "placement="
               << class_mask_name(statistics.class_mask) << '\n';
    }

    output << "cycles=" << report.makespan_cycles << '\n'
           << "reads=" << model.read_attempts << '\n'
           << "read_hits=" << model.useful_reads << '\n'
           << "read_misses=" << model.read_attempts - model.useful_reads
           << '\n'
           << "writes=" << model.write_attempts << '\n'
           << "refreshes=" << model.refreshes << '\n'
           << "migrations=" << model.migrations << '\n'
           << "invalidations=" << replay.invalidated_lines << '\n'
           << "expirations=" << model.expirations << '\n'
           << "capacity_failures=" << model.capacity_failures << '\n'
           << "queue_delay_cycles=" << model.queue_delay_cycles << '\n';
    for (const auto retention_class : {RetentionClass::Ephemeral,
                                       RetentionClass::Epoch,
                                       RetentionClass::Durable}) {
        output << "bank." << to_string(retention_class)
               << ".busy_cycles="
               << model.bank_busy_cycles[class_index(retention_class)] << '\n';
    }
    output << std::fixed << std::setprecision(2)
           << "energy.useful_read_pj=" << report.energy.useful_read_pj << '\n'
           << "energy.useful_write_pj=" << report.energy.useful_write_pj
           << '\n'
           << "energy.refresh_pj=" << report.energy.refresh_pj << '\n'
           << "energy.migration_pj=" << report.energy.migration_pj << '\n'
           << "energy.controller_pj=" << report.energy.controller_pj << '\n'
           << "energy.metadata_pj=" << report.energy.metadata_pj << '\n'
           << "energy.ecc_pj=" << report.energy.ecc_pj << '\n'
           << "energy.static_pj=" << report.energy.static_pj << '\n'
           << "modeled_dynamic_memory_energy_pj="
           << report.energy.useful_read_pj +
                  report.energy.useful_write_pj +
                  report.energy.refresh_pj +
                  report.energy.migration_pj
           << '\n'
           << "energy_pj=" << report.energy.total() << '\n';
}

struct LifetimeAggregate {
    std::uint64_t allocations = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    struct Sample {
        std::uint64_t required_cycles = 0;
        std::uint64_t write_bytes = 0;
    };
    std::vector<Sample> samples;
};

struct LifetimeLineState {
    bool initialized = false;
    std::uint64_t write_cycle = 0;
    std::uint64_t maximum_read_age = 0;
    std::uint64_t write_bytes = 0;
};

struct LifetimeAllocationState {
    std::uint64_t size = 0;
    std::string name;
    std::vector<LifetimeLineState> lines;
};

std::uint64_t lifetime_percentile(
                                  std::vector<LifetimeAggregate::Sample> values,
                                  double fraction) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end(), [](const auto& left,
                                               const auto& right) {
        return left.required_cycles < right.required_cycles;
    });
    const auto total_bytes = std::accumulate(
        values.begin(), values.end(), std::uint64_t{0},
        [](std::uint64_t total, const auto& sample) {
            return total + sample.write_bytes;
        });
    const auto target = static_cast<std::uint64_t>(
        std::ceil(fraction * static_cast<double>(total_bytes)));
    std::uint64_t accumulated = 0;
    for (const auto& sample : values) {
        accumulated += sample.write_bytes;
        if (accumulated >= std::max<std::uint64_t>(1, target)) {
            return sample.required_cycles;
        }
    }
    return values.back().required_cycles;
}

std::uint64_t lifetime_write_bytes(const LifetimeAggregate& aggregate) {
    return std::accumulate(
        aggregate.samples.begin(),
        aggregate.samples.end(),
        std::uint64_t{0},
        [](std::uint64_t total, const auto& sample) {
            return total + sample.write_bytes;
        });
}

void finish_lifetime_version(LifetimeLineState& line,
                             LifetimeAggregate& structure,
                             LifetimeAggregate& total) {
    if (!line.initialized) {
        return;
    }
    const LifetimeAggregate::Sample sample{line.maximum_read_age,
                                           line.write_bytes};
    structure.samples.push_back(sample);
    total.samples.push_back(sample);
    line = {};
}

std::string csv_field(const std::string& text) {
    if (text.find_first_of(",\"\n") == std::string::npos) {
        return text;
    }
    std::string result = "\"";
    for (const char character : text) {
        if (character == '"') {
            result += "\"\"";
        } else {
            result.push_back(character);
        }
    }
    return result + '"';
}

void write_lifetime_csv(
    const std::string& path,
    const std::map<std::string, LifetimeAggregate>& structures,
    const LifetimeAggregate& total,
    const std::vector<std::uint64_t>& thresholds) {
    if (path.empty()) {
        return;
    }
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open lifetime CSV: " + path);
    }
    output << "structure,allocations,read_bytes,write_bytes,line_versions,"
              "write_version_bytes,p50_cycles,p90_cycles,p95_cycles,"
              "p99_cycles,max_cycles,threshold_cycles,eligible_versions,"
              "eligible_write_bytes,eligible_write_percent\n";
    output << std::fixed << std::setprecision(6);
    const auto write_rows = [&](const std::string& name,
                                const LifetimeAggregate& aggregate) {
        const auto count = aggregate.samples.size();
        const auto version_bytes = lifetime_write_bytes(aggregate);
        const auto maximum = count == 0
                                 ? 0
                                 : std::max_element(
                                       aggregate.samples.begin(),
                                       aggregate.samples.end(),
                                       [](const auto& left, const auto& right) {
                                           return left.required_cycles <
                                                  right.required_cycles;
                                       })->required_cycles;
        for (const auto threshold : thresholds) {
            const auto eligible = static_cast<std::uint64_t>(std::count_if(
                aggregate.samples.begin(),
                aggregate.samples.end(),
                [&](const auto& sample) {
                    return sample.required_cycles <= threshold;
                }));
            const auto eligible_bytes = std::accumulate(
                aggregate.samples.begin(),
                aggregate.samples.end(),
                std::uint64_t{0},
                [&](std::uint64_t bytes, const auto& sample) {
                    return bytes + (sample.required_cycles <= threshold
                                        ? sample.write_bytes
                                        : 0);
                });
            const auto percent = version_bytes == 0
                                     ? 0.0
                                     : 100.0 *
                                           static_cast<double>(eligible_bytes) /
                                           static_cast<double>(version_bytes);
            output << csv_field(name) << ',' << aggregate.allocations << ','
                   << aggregate.read_bytes << ',' << aggregate.write_bytes
                   << ',' << count << ',' << version_bytes << ','
                   << lifetime_percentile(aggregate.samples, 0.50)
                   << ','
                   << lifetime_percentile(aggregate.samples, 0.90)
                   << ','
                   << lifetime_percentile(aggregate.samples, 0.95)
                   << ','
                   << lifetime_percentile(aggregate.samples, 0.99)
                   << ',' << maximum << ',' << threshold << ',' << eligible
                   << ',' << eligible_bytes << ',' << percent << '\n';
        }
    };
    write_rows("ALL", total);
    for (const auto& entry : structures) {
        write_rows(entry.first, entry.second);
    }
}

void write_lifetime_json(
    const std::string& path,
    const std::string& trace_path,
    const std::string& fingerprint,
    const DeviceProfile& profile,
    const std::map<std::string, LifetimeAggregate>& structures,
    const LifetimeAggregate& total,
    const std::vector<std::uint64_t>& thresholds) {
    if (path.empty()) {
        return;
    }
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open lifetime JSON: " + path);
    }
    output << "{\n  \"schema\": \"RTMEM_LIFETIME_ANALYSIS_1\",\n"
           << "  \"trace\": \"" << json_escape(trace_path) << "\",\n"
           << "  \"trace_fingerprint\": \"" << fingerprint << "\",\n"
           << "  \"profile_id\": \"" << json_escape(profile.profile_id)
           << "\",\n"
           << "  \"timing_model\": "
              "\"conservative_serial_max_access_latency\",\n"
           << "  \"thresholds\": [";
    for (std::size_t index = 0; index < thresholds.size(); ++index) {
        output << thresholds[index]
               << (index + 1 == thresholds.size() ? "" : ", ");
    }
    output << "],\n  \"structures\": {\n";

    std::vector<std::pair<std::string, const LifetimeAggregate*>> entries;
    entries.emplace_back("ALL", &total);
    for (const auto& entry : structures) {
        entries.emplace_back(entry.first, &entry.second);
    }
    output << std::fixed << std::setprecision(6);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& name = entries[index].first;
        const auto& aggregate = *entries[index].second;
        const auto count = aggregate.samples.size();
        const auto version_bytes = lifetime_write_bytes(aggregate);
        const auto maximum = count == 0
                                 ? 0
                                 : std::max_element(
                                       aggregate.samples.begin(),
                                       aggregate.samples.end(),
                                       [](const auto& left, const auto& right) {
                                           return left.required_cycles <
                                                  right.required_cycles;
                                       })->required_cycles;
        output << "    \"" << json_escape(name) << "\": {"
               << "\"allocations\": " << aggregate.allocations
               << ", \"read_bytes\": " << aggregate.read_bytes
               << ", \"write_bytes\": " << aggregate.write_bytes
               << ", \"line_versions\": " << count
               << ", \"write_version_bytes\": " << version_bytes
               << ", \"p50_cycles\": "
               << lifetime_percentile(aggregate.samples, 0.50)
               << ", \"p90_cycles\": "
               << lifetime_percentile(aggregate.samples, 0.90)
               << ", \"p95_cycles\": "
               << lifetime_percentile(aggregate.samples, 0.95)
               << ", \"p99_cycles\": "
               << lifetime_percentile(aggregate.samples, 0.99)
               << ", \"max_cycles\": " << maximum
               << ", \"eligible_write_percent\": {";
        for (std::size_t threshold_index = 0;
             threshold_index < thresholds.size();
             ++threshold_index) {
            const auto threshold = thresholds[threshold_index];
            const auto eligible_bytes = std::accumulate(
                aggregate.samples.begin(),
                aggregate.samples.end(),
                std::uint64_t{0},
                [&](std::uint64_t bytes, const auto& sample) {
                    return bytes + (sample.required_cycles <= threshold
                                        ? sample.write_bytes
                                        : 0);
                });
            const auto percent = version_bytes == 0
                                     ? 0.0
                                     : 100.0 *
                                           static_cast<double>(eligible_bytes) /
                                           static_cast<double>(version_bytes);
            output << "\"" << threshold << "\": " << percent
                   << (threshold_index + 1 == thresholds.size() ? "" : ", ");
        }
        output << "}}" << (index + 1 == entries.size() ? "\n" : ",\n");
    }
    output << "  }\n}\n";
}

void print_lifetime_rows(
    const std::map<std::string, LifetimeAggregate>& structures,
    const LifetimeAggregate& total,
    const std::vector<std::uint64_t>& thresholds,
    std::ostream& output) {
    const auto print_one = [&](const std::string& name,
                               const LifetimeAggregate& aggregate) {
        const auto count = aggregate.samples.size();
        const auto version_bytes = lifetime_write_bytes(aggregate);
        const auto prefix = "lifetime." + name + '.';
        output << prefix << "allocations=" << aggregate.allocations << '\n'
               << prefix << "read_bytes=" << aggregate.read_bytes << '\n'
               << prefix << "write_bytes=" << aggregate.write_bytes << '\n'
               << prefix << "line_versions=" << count << '\n'
               << prefix << "write_version_bytes=" << version_bytes << '\n'
               << prefix << "p50_cycles="
               << lifetime_percentile(aggregate.samples, 0.50) << '\n'
               << prefix << "p90_cycles="
               << lifetime_percentile(aggregate.samples, 0.90) << '\n'
               << prefix << "p95_cycles="
               << lifetime_percentile(aggregate.samples, 0.95) << '\n'
               << prefix << "p99_cycles="
               << lifetime_percentile(aggregate.samples, 0.99) << '\n'
               << prefix << "max_cycles="
               << (count == 0
                       ? 0
                       : std::max_element(
                              aggregate.samples.begin(),
                              aggregate.samples.end(),
                              [](const auto& left, const auto& right) {
                                  return left.required_cycles <
                                         right.required_cycles;
                              })->required_cycles)
               << '\n';
        output << std::fixed << std::setprecision(6);
        for (const auto threshold : thresholds) {
            const auto eligible_bytes = std::accumulate(
                aggregate.samples.begin(),
                aggregate.samples.end(),
                std::uint64_t{0},
                [&](std::uint64_t bytes, const auto& sample) {
                    return bytes + (sample.required_cycles <= threshold
                                        ? sample.write_bytes
                                        : 0);
                });
            const auto percent = version_bytes == 0
                                     ? 0.0
                                     : 100.0 *
                                           static_cast<double>(eligible_bytes) /
                                           static_cast<double>(version_bytes);
            output << prefix << "eligible_write_percent." << threshold << '='
                   << percent << '\n';
        }
    };
    print_one("ALL", total);
    for (const auto& entry : structures) {
        print_one(entry.first, entry.second);
    }
}

void analyze_lifetime_events(
    const std::vector<Event>& events,
    const DeviceProfile& profile,
    std::map<std::string, LifetimeAggregate>& structures,
    LifetimeAggregate& total) {
    std::unordered_map<std::uint64_t, LifetimeAllocationState> allocations;
    std::uint64_t cycle = 0;
    const auto read_cycles = std::max(
        {profile.banks[0].read_cycles,
         profile.banks[1].read_cycles,
         profile.banks[2].read_cycles});
    const auto write_cycles = std::max(
        {profile.banks[0].write_cycles,
         profile.banks[1].write_cycles,
         profile.banks[2].write_cycles});

    for (const auto& event : events) {
        cycle = std::max(cycle, event.tick);
        if (event.kind == EventKind::Allocate) {
            if (allocations.count(event.buffer) != 0) {
                throw std::runtime_error("duplicate live buffer id");
            }
            LifetimeAllocationState state;
            state.size = event.size;
            state.name = event.name;
            state.lines.resize(aligned_lines(event.size));
            allocations.emplace(event.buffer, std::move(state));
            structures[event.name].allocations++;
            total.allocations++;
        } else if (event.kind == EventKind::Access) {
            const auto found = allocations.find(event.buffer);
            if (found == allocations.end() ||
                event.offset > found->second.size ||
                event.size > found->second.size - event.offset) {
                throw std::runtime_error(
                    "lifetime access references invalid allocation");
            }
            auto& aggregate = structures[found->second.name];
            if (event.write) {
                aggregate.write_bytes += event.size;
                total.write_bytes += event.size;
            } else {
                aggregate.read_bytes += event.size;
                total.read_bytes += event.size;
            }
            const auto range = line_range(event);
            for (auto line_index = range.first;
                 line_index <= range.second;
                 ++line_index) {
                auto& line = found->second.lines.at(line_index);
                if (event.write) {
                    finish_lifetime_version(line, aggregate, total);
                    cycle = checked_add(cycle,
                                        write_cycles,
                                        "lifetime cycle overflow");
                    line.initialized = true;
                    line.write_cycle = cycle;
                    line.maximum_read_age = 0;
                    const auto line_start = line_index * kLineBytes;
                    const auto access_end = event.offset + event.size;
                    const auto line_end = line_start + kLineBytes;
                    line.write_bytes =
                        std::min(access_end, line_end) -
                        std::max(event.offset, line_start);
                } else {
                    if (line.initialized) {
                        line.maximum_read_age = std::max(
                            line.maximum_read_age,
                            cycle - line.write_cycle);
                    }
                    cycle = checked_add(cycle,
                                        read_cycles,
                                        "lifetime cycle overflow");
                }
            }
        } else if (event.kind == EventKind::Compute) {
            cycle = checked_add(cycle,
                                event.cycles,
                                "lifetime compute cycle overflow");
        } else if (event.kind == EventKind::Free) {
            const auto found = allocations.find(event.buffer);
            if (found == allocations.end()) {
                throw std::runtime_error("free references unknown buffer");
            }
            auto& aggregate = structures[found->second.name];
            for (auto& line : found->second.lines) {
                finish_lifetime_version(line, aggregate, total);
            }
            allocations.erase(found);
        }
    }
    for (auto& allocation : allocations) {
        auto& aggregate = structures[allocation.second.name];
        for (auto& line : allocation.second.lines) {
            finish_lifetime_version(line, aggregate, total);
        }
    }
}

}  // namespace

ReplayPolicy parse_replay_policy(const std::string& text) {
    if (text == "hint") return ReplayPolicy::Hint;
    if (text == "ephemeral") return ReplayPolicy::Ephemeral;
    if (text == "epoch") return ReplayPolicy::Epoch;
    if (text == "durable") return ReplayPolicy::Durable;
    if (text == "oracle") return ReplayPolicy::Oracle;
    if (text == "refresh") return ReplayPolicy::Refresh;
    if (text == "adaptive") return ReplayPolicy::Adaptive;
    throw std::invalid_argument("unknown replay policy: " + text);
}

std::string to_string(ReplayPolicy policy) {
    switch (policy) {
        case ReplayPolicy::Hint: return "hint";
        case ReplayPolicy::Ephemeral: return "ephemeral";
        case ReplayPolicy::Epoch: return "epoch";
        case ReplayPolicy::Durable: return "durable";
        case ReplayPolicy::Oracle: return "oracle";
        case ReplayPolicy::Refresh: return "refresh";
        case ReplayPolicy::Adaptive: return "adaptive";
    }
    throw std::invalid_argument("invalid replay policy");
}

bool is_workload_trace(const std::string& path) {
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = tokens(line);
        if (!fields.empty()) {
            return fields.size() == 2 && fields[0] == "RTTRACE" &&
                   fields[1] == "2";
        }
    }
    return false;
}

bool analyze_workload_lifetimes(const std::string& path,
                                const DeviceProfile& profile,
                                const LifetimeAnalysisOptions& options,
                                std::ostream& output,
                                std::ostream& errors) {
    try {
        auto thresholds = options.thresholds;
        if (thresholds.empty()) {
            throw std::invalid_argument("lifetime thresholds cannot be empty");
        }
        std::sort(thresholds.begin(), thresholds.end());
        thresholds.erase(std::unique(thresholds.begin(), thresholds.end()),
                         thresholds.end());
        const auto events = load_events(path);
        std::map<std::string, LifetimeAggregate> structures;
        LifetimeAggregate total;
        analyze_lifetime_events(events, profile, structures, total);
        const auto fingerprint = fnv1a64(read_file(path));
        output << "workload_trace=" << path << '\n'
               << "trace_fingerprint=" << fingerprint << '\n'
               << "profile_id=" << profile.profile_id << '\n'
               << "timing_model=conservative_serial_max_access_latency\n"
               << "analysis=lifetime status=PASS\n";
        print_lifetime_rows(structures, total, thresholds, output);
        write_lifetime_csv(options.csv_path,
                           structures,
                           total,
                           thresholds);
        write_lifetime_json(options.json_path,
                            path,
                            fingerprint,
                            profile,
                            structures,
                            total,
                            thresholds);
        return true;
    } catch (const std::exception& error) {
        errors << error.what() << '\n';
        return false;
    }
}

ReplayOutcome run_workload_trace(const std::string& path,
                                 ReplayPolicy policy,
                                 ErrorMode mode,
                                 std::uint64_t seed,
                                 const DeviceProfile& profile,
                                 const ReplayOptions& options,
                                 std::ostream& output,
                                 std::ostream& errors) {
    try {
        const auto events = load_events(path);
        const auto report = replay_events(path,
                                          events,
                                          policy,
                                          mode,
                                          seed,
                                          profile,
                                          options);
        print_report(path, policy, profile, events, report, output);
        if (!options.json_path.empty()) {
            write_json(options.json_path,
                       path,
                       policy,
                       profile,
                       events,
                       report);
        }
        return {true, report.replay.unsafe_accesses == 0};
    } catch (const std::exception& error) {
        errors << error.what() << '\n';
        return {false, false};
    }
}

}  // namespace rtmem
