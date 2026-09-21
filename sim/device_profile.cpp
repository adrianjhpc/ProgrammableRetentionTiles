#include "device_profile.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rtmem {
namespace {

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

std::uint64_t integer(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value;
}

double real(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value;
}

std::size_t index(RetentionClass retention_class) {
    return static_cast<std::size_t>(retention_class);
}

}  // namespace

BankConfigs load_device_profile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open device profile: " + path);
    }

    BankConfigs result{};
    std::array<bool, 3> seen{};
    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;

    while (std::getline(input, line)) {
        ++line_number;
        const auto fields = tokens(line);
        if (fields.empty()) {
            continue;
        }
        try {
            if (!header_seen) {
                if (fields.size() != 2 || fields[0] != "RTMEM_PROFILE" ||
                    fields[1] != "1") {
                    throw std::runtime_error(
                        "expected RTMEM_PROFILE 1 header");
                }
                header_seen = true;
                continue;
            }
            if (fields.size() != 7) {
                throw std::runtime_error(
                    "profile row requires seven fields");
            }
            const auto retention_class = parse_retention_class(fields[0]);
            const auto selected = index(retention_class);
            if (seen[selected]) {
                throw std::runtime_error("duplicate retention class");
            }
            seen[selected] = true;

            BankConfig config;
            config.name = to_string(retention_class);
            const auto capacity = integer(fields[1], "capacity");
            if (capacity > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("capacity exceeds host size_t");
            }
            config.capacity_lines = static_cast<std::size_t>(capacity);
            config.retention_cycles =
                fields[2] == "INFINITE"
                    ? std::numeric_limits<std::uint64_t>::max()
                    : integer(fields[2], "retention");
            config.read_cycles = integer(fields[3], "read cycles");
            config.write_cycles = integer(fields[4], "write cycles");
            config.read_energy_pj = real(fields[5], "read energy");
            config.write_energy_pj = real(fields[6], "write energy");
            if (config.capacity_lines == 0 || config.retention_cycles == 0 ||
                config.read_cycles == 0 || config.write_cycles == 0) {
                throw std::runtime_error(
                    "capacity, retention, and latency must be nonzero");
            }
            result[selected] = std::move(config);
        } catch (const std::exception& error) {
            std::ostringstream message;
            message << path << ':' << line_number << ": " << error.what();
            throw std::runtime_error(message.str());
        }
    }

    if (!header_seen || !seen[0] || !seen[1] || !seen[2]) {
        throw std::runtime_error(
            "device profile must define all three retention classes");
    }
    return result;
}

}  // namespace rtmem
