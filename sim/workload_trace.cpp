#include "workload_trace.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rtmem {
namespace {

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
    RetentionClass retention_class = RetentionClass::Epoch;
    std::string name;
    bool live = false;
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

struct ReplayStatistics {
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    std::uint64_t access_events = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t compute_cycles = 0;
    std::uint64_t phases = 0;
    std::uint64_t barriers = 0;
    std::uint64_t unsafe_accesses = 0;
    std::array<ClassStatistics, 3> classes{};
    std::map<std::string, StructureStatistics> structures;
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

std::vector<Event> load_events(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open workload trace: " + path);
    }

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
            if (!events.empty() && event.sequence != previous_sequence + 1) {
                throw std::runtime_error("non-contiguous event sequence");
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
    if (event.offset > std::numeric_limits<std::uint64_t>::max() -
                           (event.size - 1)) {
        throw std::runtime_error("access range overflow");
    }
    return {event.offset / kLineBytes,
            (event.offset + event.size - 1) / kLineBytes};
}

std::unordered_map<std::uint64_t, std::uint64_t> oracle_ages(
    const std::vector<Event>& events,
    const RetentionMemory& memory) {
    struct AgeState {
        std::uint64_t size = 0;
        std::vector<std::uint64_t> last_write;
    };
    std::unordered_map<std::uint64_t, AgeState> states;
    std::unordered_map<std::uint64_t, std::uint64_t> result;
    std::uint64_t cycle = 0;
    const auto maximum_read_cycles = std::max(
        {memory.bank_config(RetentionClass::Ephemeral).read_cycles,
         memory.bank_config(RetentionClass::Epoch).read_cycles,
         memory.bank_config(RetentionClass::Durable).read_cycles});
    const auto maximum_write_cycles = std::max(
        {memory.bank_config(RetentionClass::Ephemeral).write_cycles,
         memory.bank_config(RetentionClass::Epoch).write_cycles,
         memory.bank_config(RetentionClass::Durable).write_cycles});

    const auto advance_cycle = [&cycle](std::uint64_t increment) {
        if (cycle > std::numeric_limits<std::uint64_t>::max() - increment) {
            throw std::runtime_error("workload cycle overflow");
        }
        cycle += increment;
    };

    for (const auto& event : events) {
        if (event.kind == EventKind::Allocate) {
            const auto lines = (event.size + kLineBytes - 1) / kLineBytes;
            AgeState state;
            state.size = event.size;
            state.last_write.resize(lines);
            for (auto& last_write : state.last_write) {
                advance_cycle(maximum_write_cycles);
                last_write = cycle;
            }
            states[event.buffer] = std::move(state);
        } else if (event.kind == EventKind::Access) {
            auto found = states.find(event.buffer);
            if (found == states.end() ||
                event.offset > found->second.size ||
                event.size > found->second.size - event.offset) {
                throw std::runtime_error("access references invalid allocation");
            }
            const auto range = line_range(event);
            for (auto line = range.first; line <= range.second; ++line) {
                if (event.write) {
                    advance_cycle(maximum_write_cycles);
                    found->second.last_write.at(line) = cycle;
                } else {
                    result[event.buffer] = std::max(
                        result[event.buffer],
                        cycle - found->second.last_write.at(line));
                    advance_cycle(maximum_read_cycles);
                }
            }
        } else if (event.kind == EventKind::Compute) {
            advance_cycle(event.cycles);
        } else if (event.kind == EventKind::Free) {
            states.erase(event.buffer);
        }
    }
    return result;
}

RetentionClass oracle_class(std::uint64_t age,
                            const RetentionMemory& memory) {
    if (age < memory.bank_config(RetentionClass::Ephemeral).retention_cycles) {
        return RetentionClass::Ephemeral;
    }
    if (age < memory.bank_config(RetentionClass::Epoch).retention_cycles) {
        return RetentionClass::Epoch;
    }
    return RetentionClass::Durable;
}

RetentionClass selected_class(
    ReplayPolicy policy,
    const Event& allocation,
    const std::unordered_map<std::uint64_t, std::uint64_t>& ages,
    const RetentionMemory& memory) {
    switch (policy) {
        case ReplayPolicy::Hint:
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
                                memory);
        }
    }
    throw std::runtime_error("invalid replay policy");
}

std::uint64_t aligned_lines(std::uint64_t size) {
    if (size > std::numeric_limits<std::uint64_t>::max() -
                   (kLineBytes - 1)) {
        throw std::runtime_error("allocation size overflow");
    }
    return (size + kLineBytes - 1) / kLineBytes;
}

}  // namespace

ReplayPolicy parse_replay_policy(const std::string& text) {
    if (text == "hint") {
        return ReplayPolicy::Hint;
    }
    if (text == "ephemeral") {
        return ReplayPolicy::Ephemeral;
    }
    if (text == "epoch") {
        return ReplayPolicy::Epoch;
    }
    if (text == "durable") {
        return ReplayPolicy::Durable;
    }
    if (text == "oracle") {
        return ReplayPolicy::Oracle;
    }
    throw std::invalid_argument("unknown replay policy: " + text);
}

std::string to_string(ReplayPolicy policy) {
    switch (policy) {
        case ReplayPolicy::Hint:
            return "hint";
        case ReplayPolicy::Ephemeral:
            return "ephemeral";
        case ReplayPolicy::Epoch:
            return "epoch";
        case ReplayPolicy::Durable:
            return "durable";
        case ReplayPolicy::Oracle:
            return "oracle";
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

bool run_workload_trace(const std::string& path,
                        ReplayPolicy policy,
                        ErrorMode mode,
                        std::uint64_t seed,
                        const BankConfigs& configs,
                        std::ostream& output,
                        std::ostream& errors) {
    try {
        const auto events = load_events(path);
        RetentionMemory memory(configs, mode, seed);
        const auto ages = oracle_ages(events, memory);
        std::unordered_map<std::uint64_t, Allocation> allocations;
        ReplayStatistics replay;
        std::uint64_t next_base = 0;

        for (const auto& event : events) {
            if (event.kind == EventKind::Allocate) {
                if (allocations.count(event.buffer) != 0) {
                    throw std::runtime_error("duplicate live buffer id");
                }
                const auto lines = aligned_lines(event.size);
                if (next_base > std::numeric_limits<std::uint64_t>::max() -
                                    lines * kLineBytes) {
                    throw std::runtime_error("logical address overflow");
                }
                Allocation allocation;
                allocation.base = next_base;
                allocation.size = event.size;
                allocation.retention_class =
                    selected_class(policy, event, ages, memory);
                allocation.name = event.name;
                allocation.live = true;
                next_base += lines * kLineBytes;
                allocations.emplace(event.buffer, allocation);
                ++replay.allocations;

                auto& class_statistics =
                    replay.classes.at(class_index(allocation.retention_class));
                ++class_statistics.allocations;
                class_statistics.allocated_bytes += allocation.size;
                add_live_bytes(replay,
                               allocation.retention_class,
                               allocation.size);
                auto& structure = replay.structures[allocation.name];
                ++structure.allocations;
                structure.allocated_bytes += allocation.size;
                structure.class_mask |= class_bit(allocation.retention_class);

                for (std::uint64_t line = 0; line < lines; ++line) {
                    const auto initialized = memory.write(
                        allocation.base + line * kLineBytes,
                        allocation.retention_class,
                        0);
                    if (!initialized.ok) {
                        ++replay.unsafe_accesses;
                    }
                }
            } else if (event.kind == EventKind::Free) {
                const auto found = allocations.find(event.buffer);
                if (found == allocations.end()) {
                    throw std::runtime_error("free references unknown buffer");
                }
                const auto lines = aligned_lines(found->second.size);
                for (std::uint64_t line = 0; line < lines; ++line) {
                    memory.invalidate(found->second.base +
                                      line * kLineBytes);
                }
                remove_live_bytes(replay,
                                  found->second.retention_class,
                                  found->second.size);
                allocations.erase(found);
                ++replay.frees;
            } else if (event.kind == EventKind::Hint) {
                const auto found = allocations.find(event.buffer);
                if (found == allocations.end()) {
                    throw std::runtime_error("hint references unknown buffer");
                }
                if (policy == ReplayPolicy::Hint &&
                    found->second.retention_class != event.hint) {
                    const auto previous_class =
                        found->second.retention_class;
                    const auto lines = aligned_lines(found->second.size);
                    for (std::uint64_t line = 0; line < lines; ++line) {
                        const auto migrated = memory.migrate(
                            found->second.base + line * kLineBytes,
                            event.hint);
                        if (!migrated.ok) {
                            ++replay.unsafe_accesses;
                        }
                    }
                    remove_live_bytes(replay,
                                      previous_class,
                                      found->second.size);
                    add_live_bytes(replay,
                                   event.hint,
                                   found->second.size);
                    found->second.retention_class = event.hint;
                    replay.structures[found->second.name].class_mask |=
                        class_bit(event.hint);
                }
            } else if (event.kind == EventKind::Access) {
                const auto found = allocations.find(event.buffer);
                if (found == allocations.end() ||
                    event.offset > found->second.size ||
                    event.size > found->second.size - event.offset) {
                    throw std::runtime_error(
                        "access references unknown buffer or exceeds bounds");
                }
                const auto range = line_range(event);
                for (auto line = range.first; line <= range.second; ++line) {
                    const auto address =
                        found->second.base + line * kLineBytes;
                    const auto result =
                        event.write
                            ? memory.write(address,
                                           found->second.retention_class,
                                           event.sequence)
                            : memory.read(address);
                    if (!result.ok) {
                        ++replay.unsafe_accesses;
                    }
                }
                ++replay.access_events;
                auto& class_statistics = replay.classes.at(
                    class_index(found->second.retention_class));
                auto& structure =
                    replay.structures[found->second.name];
                structure.class_mask |=
                    class_bit(found->second.retention_class);
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
                memory.advance(event.cycles);
                replay.compute_cycles += event.cycles;
            } else if (event.kind == EventKind::Phase) {
                ++replay.phases;
            } else if (event.kind == EventKind::Barrier) {
                ++replay.barriers;
            }
        }

        output << "workload_trace=" << path << '\n'
               << "policy=" << to_string(policy) << '\n'
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
               << "status="
               << (replay.unsafe_accesses == 0 ? "SAFE" : "UNSAFE")
               << '\n';
        for (const auto retention_class : {RetentionClass::Ephemeral,
                                           RetentionClass::Epoch,
                                           RetentionClass::Durable}) {
            const auto& statistics =
                replay.classes.at(class_index(retention_class));
            const auto prefix = "class." + to_string(retention_class) + '.';
            output << prefix << "allocations=" << statistics.allocations
                   << '\n'
                   << prefix << "allocated_bytes="
                   << statistics.allocated_bytes << '\n'
                   << prefix << "peak_live_bytes="
                   << statistics.peak_live_bytes << '\n'
                   << prefix << "read_bytes=" << statistics.read_bytes
                   << '\n'
                   << prefix << "write_bytes=" << statistics.write_bytes
                   << '\n';
        }
        for (const auto& entry : replay.structures) {
            const auto prefix = "structure." + entry.first + '.';
            const auto& statistics = entry.second;
            output << prefix << "allocations=" << statistics.allocations
                   << '\n'
                   << prefix << "allocated_bytes="
                   << statistics.allocated_bytes << '\n'
                   << prefix << "read_bytes=" << statistics.read_bytes
                   << '\n'
                   << prefix << "write_bytes=" << statistics.write_bytes
                   << '\n'
                   << prefix << "placement="
                   << class_mask_name(statistics.class_mask) << '\n';
        }
        memory.print_statistics(output);
        return true;
    } catch (const std::exception& error) {
        errors << error.what() << '\n';
        return false;
    }
}

}  // namespace rtmem
