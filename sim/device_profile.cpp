#include "device_profile.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
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
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
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

void validate_bank(const BankConfig& config) {
    if (config.capacity_lines == 0 || config.retention_cycles == 0 ||
        config.read_cycles == 0 || config.write_cycles == 0) {
        throw std::runtime_error(
            "capacity, retention, and latency must be nonzero");
    }
}

void set_metadata(DeviceProfile& profile,
                  const std::string& key,
                  const std::string& value) {
    if (key == "profile_id") {
        profile.profile_id = value;
    } else if (key == "source") {
        profile.source = value;
    } else if (key == "tick_ns") {
        profile.tick_ns = real(value, "tick_ns");
        if (profile.tick_ns == 0.0) {
            throw std::runtime_error("tick_ns must be nonzero");
        }
    } else if (key == "line_bytes") {
        profile.line_bytes = integer(value, "line_bytes");
        if (profile.line_bytes != kLineBytes) {
            throw std::runtime_error(
                "this simulator build requires line_bytes=64");
        }
    } else if (key == "controller_access_pj") {
        profile.controller_access_pj = real(value, key.c_str());
    } else if (key == "metadata_access_pj") {
        profile.metadata_access_pj = real(value, key.c_str());
    } else if (key == "ecc_read_pj") {
        profile.ecc_read_pj = real(value, key.c_str());
    } else if (key == "ecc_write_pj") {
        profile.ecc_write_pj = real(value, key.c_str());
    } else if (key == "migration_setup_pj") {
        profile.migration_setup_pj = real(value, key.c_str());
    } else if (key == "migration_setup_cycles") {
        profile.migration_setup_cycles = integer(value, key.c_str());
    } else {
        throw std::runtime_error("unknown META key: " + key);
    }
}

}  // namespace

DeviceProfile default_device_profile() {
    DeviceProfile profile;
    profile.banks = default_bank_configs();
    return profile;
}

DeviceProfile load_device_profile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open device profile: " + path);
    }

    std::ostringstream raw;
    raw << input.rdbuf();
    const std::string contents = raw.str();
    std::istringstream lines(contents);

    DeviceProfile result = default_device_profile();
    result.profile_id = path;
    result.source = "unspecified";
    result.fingerprint = fnv1a64(contents);
    result.banks = BankConfigs{};

    std::array<bool, 3> seen{};
    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;
    bool profile_id_seen = false;
    bool source_seen = false;
    std::set<std::string> metadata_keys;

    while (std::getline(lines, line)) {
        ++line_number;
        const auto fields = tokens(line);
        if (fields.empty()) {
            continue;
        }
        try {
            if (!header_seen) {
                if (fields.size() != 2 || fields[0] != "RTMEM_PROFILE" ||
                    (fields[1] != "1" && fields[1] != "2")) {
                    throw std::runtime_error(
                        "expected RTMEM_PROFILE 1 or RTMEM_PROFILE 2 header");
                }
                result.version = static_cast<int>(integer(fields[1], "version"));
                header_seen = true;
                continue;
            }

            if (result.version == 2 && fields[0] == "META") {
                if (fields.size() != 3) {
                    throw std::runtime_error(
                        "META row requires a key and one whitespace-free value");
                }
                if (!metadata_keys.insert(fields[1]).second) {
                    throw std::runtime_error("duplicate META key: " +
                                             fields[1]);
                }
                set_metadata(result, fields[1], fields[2]);
                profile_id_seen = profile_id_seen ||
                                  fields[1] == "profile_id";
                source_seen = source_seen || fields[1] == "source";
                continue;
            }

            const std::size_t first =
                result.version == 2 && fields[0] == "BANK" ? 1 : 0;
            const std::size_t expected_fields =
                result.version == 2 ? 9 + first : 7;
            if (fields.size() != expected_fields) {
                throw std::runtime_error(
                    result.version == 2
                        ? "BANK row requires class, capacity, retention, read/write cycles, read/write pJ, static mW, and channels"
                        : "profile row requires seven fields");
            }

            const auto retention_class = parse_retention_class(fields[first]);
            const auto selected = index(retention_class);
            if (seen[selected]) {
                throw std::runtime_error("duplicate retention class");
            }
            seen[selected] = true;

            BankConfig config;
            config.name = to_string(retention_class);
            const auto capacity = integer(fields[first + 1], "capacity");
            if (capacity > std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("capacity exceeds host size_t");
            }
            config.capacity_lines = static_cast<std::size_t>(capacity);
            config.retention_cycles =
                fields[first + 2] == "INFINITE"
                    ? std::numeric_limits<std::uint64_t>::max()
                    : integer(fields[first + 2], "retention");
            config.read_cycles = integer(fields[first + 3], "read cycles");
            config.write_cycles = integer(fields[first + 4], "write cycles");
            config.read_energy_pj = real(fields[first + 5], "read energy");
            config.write_energy_pj = real(fields[first + 6], "write energy");
            validate_bank(config);
            result.banks[selected] = std::move(config);

            if (result.version == 2) {
                result.static_power_mw[selected] =
                    real(fields[first + 7], "static power");
                const auto channels = integer(fields[first + 8], "channels");
                if (channels == 0 ||
                    channels > std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error(
                        "channels must fit uint32 and be nonzero");
                }
                result.channels[selected] = static_cast<std::uint32_t>(channels);
            }
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
    if (result.version == 2 &&
        (!profile_id_seen || !source_seen || result.profile_id.empty() ||
         result.source.empty())) {
        throw std::runtime_error(
            "RTMEM_PROFILE 2 requires nonempty profile_id and source metadata");
    }
    return result;
}

}  // namespace rtmem
