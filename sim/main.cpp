#include "retention_model.hpp"
#include "device_profile.hpp"
#include "workload_trace.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rtmem::ErrorMode;
using rtmem::RetentionClass;
using rtmem::RetentionMemory;

std::uint64_t parse_number(const std::string& text) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
        throw std::invalid_argument("invalid number: " + text);
    }
    return value;
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

bool run_trace(const std::string& path,
               ErrorMode mode,
               std::uint64_t seed,
               const rtmem::BankConfigs& configs) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open trace: " + path);
    }

    RetentionMemory memory(configs, mode, seed);
    std::string line;
    std::size_t line_number = 0;
    std::size_t checks = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const auto fields = tokens(line);
        if (fields.empty()) {
            continue;
        }

        try {
            const auto& command = fields[0];
            if (command == "WRITE" && fields.size() == 4) {
                const auto result = memory.write(
                    parse_number(fields[1]),
                    rtmem::parse_retention_class(fields[2]),
                    parse_number(fields[3]));
                if (!result.ok) {
                    throw std::runtime_error(result.message);
                }
            } else if (command == "READ" && fields.size() >= 3) {
                const auto result = memory.read(parse_number(fields[1]));
                if (fields[2] == "MISS") {
                    ++checks;
                    if (result.ok) {
                        throw std::runtime_error("expected a miss but read hit");
                    }
                } else if (fields[2] == "EXPECT" && fields.size() == 4) {
                    ++checks;
                    const auto expected = parse_number(fields[3]);
                    if (!result.ok || result.value != expected) {
                        std::ostringstream message;
                        message << "read mismatch: expected 0x" << std::hex
                                << expected;
                        throw std::runtime_error(message.str());
                    }
                } else {
                    throw std::runtime_error("READ requires MISS or EXPECT");
                }
            } else if (command == "WAIT" && fields.size() == 2) {
                memory.advance(parse_number(fields[1]));
            } else if (command == "REFRESH" && fields.size() == 2) {
                const auto result = memory.refresh(parse_number(fields[1]));
                if (!result.ok) {
                    throw std::runtime_error(result.message);
                }
            } else if (command == "MIGRATE" && fields.size() == 3) {
                const auto result = memory.migrate(
                    parse_number(fields[1]),
                    rtmem::parse_retention_class(fields[2]));
                if (!result.ok) {
                    throw std::runtime_error(result.message);
                }
            } else if (command == "INVALIDATE" && fields.size() == 2) {
                const auto result = memory.invalidate(parse_number(fields[1]));
                if (!result.ok) {
                    throw std::runtime_error(result.message);
                }
            } else if (command == "STATS" && fields.size() == 1) {
                memory.print_statistics(std::cout);
            } else {
                throw std::runtime_error("unknown command or wrong field count");
            }
        } catch (const std::exception& error) {
            std::cerr << path << ':' << line_number << ": " << error.what()
                      << '\n';
            return false;
        }
    }

    std::cout << "trace=" << path << " checks=" << checks << " status=PASS\n";
    memory.print_statistics(std::cout);
    return true;
}

bool self_test() {
    RetentionMemory memory;
    constexpr std::uint64_t address = 0x1000;
    constexpr std::uint64_t value = 0x0123456789abcdefULL;

    if (!memory.write(address, RetentionClass::Ephemeral, value).ok) {
        return false;
    }
    memory.advance(100);
    const auto live = memory.read(address);
    if (!live.ok || live.value != value) {
        return false;
    }
    memory.advance(30);
    const auto expired = memory.read(address);
    if (expired.ok || !expired.expired) {
        return false;
    }

    if (!memory.write(address, RetentionClass::Ephemeral, value).ok) {
        return false;
    }
    memory.advance(100);
    if (!memory.refresh(address).ok) {
        return false;
    }
    memory.advance(100);
    if (!memory.read(address).ok) {
        return false;
    }
    if (!memory.migrate(address, RetentionClass::Epoch).ok) {
        return false;
    }
    memory.advance(500);
    if (!memory.read(address).ok) {
        return false;
    }

    // Durable data must not expire in the architectural model.
    constexpr std::uint64_t durable_address = 0x2000;
    if (!memory.write(durable_address, RetentionClass::Durable, value).ok) {
        return false;
    }
    memory.advance(1'000'000);
    if (!memory.read(durable_address).ok) {
        return false;
    }

    // All byte addresses in a 64-byte line refer to the same logical line.
    constexpr std::uint64_t alias_address = 0x303f;
    if (!memory.write(0x3000, RetentionClass::Epoch, 0xa5a5).ok) {
        return false;
    }
    const auto alias_read = memory.read(alias_address);
    if (!alias_read.ok || alias_read.value != 0xa5a5) {
        return false;
    }

    // A full bank rejects a new line without losing existing contents.
    RetentionMemory capacity_memory;
    for (std::uint64_t line = 0; line < 64; ++line) {
        if (!capacity_memory
                 .write(line * rtmem::kLineBytes,
                        RetentionClass::Ephemeral,
                        line)
                 .ok) {
            return false;
        }
    }
    if (capacity_memory
            .write(64 * rtmem::kLineBytes,
                   RetentionClass::Ephemeral,
                   64)
            .ok) {
        return false;
    }
    if (!capacity_memory.invalidate(0).ok ||
        !capacity_memory
             .write(64 * rtmem::kLineBytes,
                    RetentionClass::Ephemeral,
                    64)
             .ok) {
        return false;
    }

    std::cout << "self_test=PASS\n";
    return true;
}

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " --self-test | --trace FILE [--mode deterministic|stochastic]"
                 " [--seed N] [--policy hint|ephemeral|epoch|durable|oracle]"
                 " [--profile FILE]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string trace_path;
    ErrorMode mode = ErrorMode::Deterministic;
    std::uint64_t seed = 1;
    rtmem::ReplayPolicy policy = rtmem::ReplayPolicy::Hint;
    std::string profile_path;
    bool run_self_test = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--self-test") {
            run_self_test = true;
        } else if (argument == "--trace" && index + 1 < argc) {
            trace_path = argv[++index];
        } else if (argument == "--mode" && index + 1 < argc) {
            const std::string selected = argv[++index];
            if (selected == "deterministic") {
                mode = ErrorMode::Deterministic;
            } else if (selected == "stochastic") {
                mode = ErrorMode::Stochastic;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (argument == "--seed" && index + 1 < argc) {
            seed = parse_number(argv[++index]);
        } else if (argument == "--policy" && index + 1 < argc) {
            policy = rtmem::parse_replay_policy(argv[++index]);
        } else if (argument == "--profile" && index + 1 < argc) {
            profile_path = argv[++index];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    try {
        if (run_self_test) {
            return self_test() ? 0 : 1;
        }
        if (!trace_path.empty()) {
            const auto configs = profile_path.empty()
                                     ? rtmem::default_bank_configs()
                                     : rtmem::load_device_profile(profile_path);
            if (rtmem::is_workload_trace(trace_path)) {
                return rtmem::run_workload_trace(trace_path,
                                                 policy,
                                                 mode,
                                                 seed,
                                                 configs,
                                                 std::cout,
                                                 std::cerr)
                           ? 0
                           : 1;
            }
            return run_trace(trace_path, mode, seed, configs) ? 0 : 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    usage(argv[0]);
    return 2;
}
