#include "rtmem/rtmem.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct Direction {
    rtmem::retention_class source;
    rtmem::retention_class destination;
    std::string name;
};

struct Options {
    std::string xclbin;
    std::string device = "0";
    std::vector<std::size_t> sizes{
        64, 4096, 65536, 1u << 20, 16u << 20};
    std::vector<Direction> directions;
    std::size_t warmup = 2;
    std::size_t iterations = 10;
    std::string verification = "full";
    std::string csv_path;
    std::string json_path;
};

struct Measurement {
    std::string direction;
    std::size_t bytes = 0;
    std::size_t iteration = 0;
    std::uint64_t source_allocation_wall_ns = 0;
    std::uint64_t source_bo_allocation_ns = 0;
    std::uint64_t source_initialization_h2d_ns = 0;
    std::uint64_t fill_wall_ns = 0;
    std::uint64_t fill_h2d_ns = 0;
    std::uint64_t migration_e2e_ns = 0;
    std::uint64_t destination_allocation_ns = 0;
    std::uint64_t submit_ns = 0;
    std::uint64_t wait_ns = 0;
    std::uint64_t verify_wall_ns = 0;
    std::uint64_t verify_d2h_ns = 0;
};

std::uint64_t elapsed_ns(clock_type::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_type::now() - start)
            .count());
}

std::uint64_t difference(std::uint64_t after, std::uint64_t before) {
    if (after < before) {
        throw std::runtime_error("backend statistic counter moved backwards");
    }
    return after - before;
}

std::string lower(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

rtmem::retention_class parse_class(const std::string& text) {
    const auto value = lower(text);
    if (value == "ephemeral" || value == "e") {
        return rtmem::retention_class::ephemeral;
    }
    if (value == "epoch" || value == "p") {
        return rtmem::retention_class::epoch;
    }
    if (value == "durable" || value == "d") {
        return rtmem::retention_class::durable;
    }
    throw std::invalid_argument("unknown retention class: " + text);
}

std::string class_name(rtmem::retention_class value) {
    switch (value) {
        case rtmem::retention_class::ephemeral:
            return "ephemeral";
        case rtmem::retention_class::epoch:
            return "epoch";
        case rtmem::retention_class::durable:
            return "durable";
    }
    throw std::invalid_argument("invalid retention class");
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string value;
    while (std::getline(input, value, delimiter)) {
        if (value.empty()) {
            throw std::invalid_argument("empty list item");
        }
        result.push_back(value);
    }
    if (result.empty()) {
        throw std::invalid_argument("empty list");
    }
    return result;
}

std::size_t parse_size(std::string text) {
    std::uint64_t multiplier = 1;
    if (!text.empty()) {
        const char suffix = text.back();
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024;
            text.pop_back();
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024 * 1024;
            text.pop_back();
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
            text.pop_back();
        }
    }
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::size_t>::max() / multiplier) {
        throw std::invalid_argument("invalid byte size: " + text);
    }
    return static_cast<std::size_t>(value * multiplier);
}

std::size_t parse_count(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() ||
        value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return static_cast<std::size_t>(value);
}

Direction parse_direction(const std::string& text) {
    auto separator = text.find("->");
    std::size_t width = 2;
    if (separator == std::string::npos) {
        separator = text.find('-');
        width = 1;
    }
    if (separator == std::string::npos) {
        throw std::invalid_argument("direction must be source-destination");
    }
    Direction result;
    result.source = parse_class(text.substr(0, separator));
    result.destination = parse_class(text.substr(separator + width));
    if (result.source == result.destination) {
        throw std::invalid_argument("migration classes must differ");
    }
    result.name = class_name(result.source) + "-" +
                  class_name(result.destination);
    return result;
}

std::vector<Direction> all_directions() {
    std::vector<Direction> result;
    for (const auto source : {rtmem::retention_class::ephemeral,
                              rtmem::retention_class::epoch,
                              rtmem::retention_class::durable}) {
        for (const auto destination : {rtmem::retention_class::ephemeral,
                                       rtmem::retention_class::epoch,
                                       rtmem::retention_class::durable}) {
            if (source != destination) {
                result.push_back({source,
                                  destination,
                                  class_name(source) + "-" +
                                      class_name(destination)});
            }
        }
    }
    return result;
}

void usage(const char* executable) {
    std::cerr
        << "usage: " << executable
        << " XCLBIN [DEVICE] [--sizes 64,4K,1M]"
           " [--directions all|e-p,p-d,...] [--warmup N]"
           " [--iterations N] [--verify full|sample|none]"
           " [--csv FILE] [--json FILE]\n";
}

Options parse_options(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument("missing xclbin path");
    }
    Options options;
    options.xclbin = argv[1];
    int index = 2;
    if (index < argc && std::string(argv[index]).rfind("--", 0) != 0) {
        options.device = argv[index++];
    }
    while (index < argc) {
        const std::string argument = argv[index++];
        if (argument == "--sizes" && index < argc) {
            options.sizes.clear();
            for (const auto& item : split(argv[index++], ',')) {
                options.sizes.push_back(parse_size(item));
            }
        } else if (argument == "--directions" && index < argc) {
            const std::string value = argv[index++];
            if (lower(value) == "all") {
                options.directions = all_directions();
            } else {
                options.directions.clear();
                for (const auto& item : split(value, ',')) {
                    options.directions.push_back(parse_direction(item));
                }
            }
        } else if (argument == "--warmup" && index < argc) {
            options.warmup = parse_count(argv[index++], "warmup count");
        } else if (argument == "--iterations" && index < argc) {
            options.iterations = parse_count(argv[index++], "iteration count");
        } else if (argument == "--verify" && index < argc) {
            options.verification = lower(argv[index++]);
            if (options.verification != "full" &&
                options.verification != "sample" &&
                options.verification != "none") {
                throw std::invalid_argument("invalid verification mode");
            }
        } else if (argument == "--csv" && index < argc) {
            options.csv_path = argv[index++];
        } else if (argument == "--json" && index < argc) {
            options.json_path = argv[index++];
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " +
                                        argument);
        }
    }
    if (options.iterations == 0) {
        throw std::invalid_argument("iterations must be nonzero");
    }
    if (options.directions.empty()) {
        options.directions = all_directions();
    }
    return options;
}

std::uint8_t expected_byte(std::size_t index) {
    const auto mixed = static_cast<std::uint64_t>(index) *
                           0x9e3779b97f4a7c15ULL +
                       0xd1b54a32d192ed03ULL;
    return static_cast<std::uint8_t>((mixed ^ (mixed >> 29U)) & 0xffU);
}

void fill_buffer(rtmem::buffer& buffer, std::size_t bytes) {
    auto* data = buffer.map_as<std::uint8_t>(RT_MAP_WRITE);
    for (std::size_t index = 0; index < bytes; ++index) {
        data[index] = expected_byte(index);
    }
    buffer.unmap();
}

void verify_buffer(rtmem::buffer& buffer,
                   std::size_t bytes,
                   const std::string& mode) {
    if (mode == "none") {
        return;
    }
    const auto* data = buffer.map_as<const std::uint8_t>(RT_MAP_READ);
    const auto verify_range = [&](std::size_t first, std::size_t last) {
        for (std::size_t index = first; index < last; ++index) {
            if (data[index] != expected_byte(index)) {
                buffer.unmap();
                throw std::runtime_error("migration data mismatch at byte " +
                                         std::to_string(index));
            }
        }
    };
    if (mode == "full" || bytes <= 8192) {
        verify_range(0, bytes);
    } else {
        verify_range(0, 4096);
        verify_range(bytes - 4096, bytes);
    }
    buffer.unmap();
}

Measurement run_one(rtmem::runtime& runtime,
                    const Direction& direction,
                    std::size_t bytes,
                    std::size_t iteration,
                    const std::string& verification) {
    Measurement result;
    result.direction = direction.name;
    result.bytes = bytes;
    result.iteration = iteration;

    rtmem::policy policy(direction.source);
    rtmem::region region(runtime, policy, "migration_benchmark");

    auto before = runtime.backend_stats();
    auto start = clock_type::now();
    auto buffer = std::make_unique<rtmem::buffer>(region, bytes, 64);
    result.source_allocation_wall_ns = elapsed_ns(start);
    auto after = runtime.backend_stats();
    result.source_bo_allocation_ns = difference(
        after.allocation_time_ns, before.allocation_time_ns);
    result.source_initialization_h2d_ns = difference(
        after.host_to_device_time_ns, before.host_to_device_time_ns);

    before = after;
    start = clock_type::now();
    fill_buffer(*buffer, bytes);
    result.fill_wall_ns = elapsed_ns(start);
    after = runtime.backend_stats();
    result.fill_h2d_ns = difference(after.host_to_device_time_ns,
                                    before.host_to_device_time_ns);

    before = after;
    start = clock_type::now();
    buffer->reclassify(direction.destination);
    result.migration_e2e_ns = elapsed_ns(start);
    after = runtime.backend_stats();
    result.destination_allocation_ns = difference(
        after.migration_destination_allocation_time_ns,
        before.migration_destination_allocation_time_ns);
    result.submit_ns = difference(after.migration_submit_time_ns,
                                  before.migration_submit_time_ns);
    result.wait_ns = difference(after.migration_wait_time_ns,
                                before.migration_wait_time_ns);
    if (difference(after.migration_calls, before.migration_calls) != 1 ||
        difference(after.migration_bytes, before.migration_bytes) != bytes) {
        throw std::runtime_error("backend did not report exactly one migration");
    }

    before = after;
    start = clock_type::now();
    verify_buffer(*buffer, bytes, verification);
    result.verify_wall_ns = elapsed_ns(start);
    after = runtime.backend_stats();
    result.verify_d2h_ns = difference(after.device_to_host_time_ns,
                                      before.device_to_host_time_ns);
    return result;
}

double gb_per_second(std::size_t bytes, std::uint64_t nanoseconds) {
    return nanoseconds == 0
               ? 0.0
               : static_cast<double>(bytes) /
                     static_cast<double>(nanoseconds);
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto position = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(values.size())));
    const auto selected = std::max<std::size_t>(1, position) - 1;
    return values[std::min(selected, values.size() - 1)];
}

void write_csv(const std::string& path,
               const std::vector<Measurement>& measurements) {
    if (path.empty()) {
        return;
    }
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open CSV output: " + path);
    }
    output << "direction,bytes,iteration,source_allocation_wall_ns,"
              "source_bo_allocation_ns,source_initialization_h2d_ns,"
              "fill_wall_ns,fill_h2d_ns,migration_e2e_ns,"
              "destination_allocation_ns,submit_ns,wait_ns,kernel_wall_ns,"
              "migration_e2e_gbps,kernel_wall_gbps,verify_wall_ns,"
              "verify_d2h_ns\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& row : measurements) {
        const auto kernel = row.submit_ns + row.wait_ns;
        output << row.direction << ',' << row.bytes << ',' << row.iteration
               << ',' << row.source_allocation_wall_ns << ','
               << row.source_bo_allocation_ns << ','
               << row.source_initialization_h2d_ns << ',' << row.fill_wall_ns
               << ',' << row.fill_h2d_ns << ',' << row.migration_e2e_ns
               << ',' << row.destination_allocation_ns << ','
               << row.submit_ns << ',' << row.wait_ns << ',' << kernel << ','
               << gb_per_second(row.bytes, row.migration_e2e_ns) << ','
               << gb_per_second(row.bytes, kernel) << ','
               << row.verify_wall_ns << ',' << row.verify_d2h_ns << '\n';
    }
}

struct Summary {
    std::string direction;
    std::size_t bytes = 0;
    std::size_t samples = 0;
    std::uint64_t median_e2e_ns = 0;
    std::uint64_t p05_e2e_ns = 0;
    std::uint64_t p95_e2e_ns = 0;
    std::uint64_t median_destination_ns = 0;
    std::uint64_t median_kernel_ns = 0;
};

std::vector<Summary> summarize(const std::vector<Measurement>& rows) {
    std::vector<Summary> result;
    for (const auto& first : rows) {
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const Summary& item) {
                return item.direction == first.direction &&
                       item.bytes == first.bytes;
            });
        if (duplicate != result.end()) {
            continue;
        }
        std::vector<std::uint64_t> e2e;
        std::vector<std::uint64_t> destination;
        std::vector<std::uint64_t> kernel;
        for (const auto& row : rows) {
            if (row.direction == first.direction && row.bytes == first.bytes) {
                e2e.push_back(row.migration_e2e_ns);
                destination.push_back(row.destination_allocation_ns);
                kernel.push_back(row.submit_ns + row.wait_ns);
            }
        }
        Summary summary;
        summary.direction = first.direction;
        summary.bytes = first.bytes;
        summary.samples = e2e.size();
        summary.median_e2e_ns = percentile(e2e, 0.50);
        summary.p05_e2e_ns = percentile(e2e, 0.05);
        summary.p95_e2e_ns = percentile(e2e, 0.95);
        summary.median_destination_ns = percentile(destination, 0.50);
        summary.median_kernel_ns = percentile(kernel, 0.50);
        result.push_back(std::move(summary));
    }
    return result;
}

std::string json_escape(const std::string& text) {
    std::string result;
    for (char character : text) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

void write_json(const std::string& path,
                const Options& options,
                const std::vector<Summary>& summaries) {
    if (path.empty()) {
        return;
    }
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open JSON output: " + path);
    }
    output << "{\n  \"schema\": \"RTMEM_XRT_MIGRATION_1\",\n"
           << "  \"xclbin\": \"" << json_escape(options.xclbin)
           << "\",\n  \"device\": \"" << json_escape(options.device)
           << "\",\n  \"warmup\": " << options.warmup
           << ",\n  \"iterations\": " << options.iterations
           << ",\n  \"verification\": \"" << options.verification
           << "\",\n  \"summaries\": [\n";
    output << std::fixed << std::setprecision(6);
    for (std::size_t index = 0; index < summaries.size(); ++index) {
        const auto& row = summaries[index];
        output << "    {\"direction\": \"" << row.direction
               << "\", \"bytes\": " << row.bytes
               << ", \"samples\": " << row.samples
               << ", \"median_e2e_ns\": " << row.median_e2e_ns
               << ", \"p05_e2e_ns\": " << row.p05_e2e_ns
               << ", \"p95_e2e_ns\": " << row.p95_e2e_ns
               << ", \"median_destination_allocation_ns\": "
               << row.median_destination_ns
               << ", \"median_kernel_wall_ns\": " << row.median_kernel_ns
               << ", \"median_e2e_gbps\": "
               << gb_per_second(row.bytes, row.median_e2e_ns)
               << ", \"median_kernel_wall_gbps\": "
               << gb_per_second(row.bytes, row.median_kernel_ns) << "}"
               << (index + 1 == summaries.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        rtmem::runtime_options runtime_options;
        runtime_options.clock(rtmem::clock_mode::manual);
        rtmem::xrt_options xrt;
        xrt.xclbin_path = options.xclbin;
        if (options.device.find(':') != std::string::npos) {
            xrt.device_bdf = options.device;
        } else {
            xrt.device_index = static_cast<std::uint32_t>(
                parse_count(options.device, "device index"));
        }
        rtmem::runtime runtime(runtime_options, xrt);

        std::vector<Measurement> measurements;
        for (const auto& direction : options.directions) {
            for (const auto bytes : options.sizes) {
                for (std::size_t index = 0; index < options.warmup; ++index) {
                    (void)run_one(runtime,
                                  direction,
                                  bytes,
                                  index,
                                  options.verification);
                }
                for (std::size_t index = 0; index < options.iterations;
                     ++index) {
                    measurements.push_back(run_one(runtime,
                                                   direction,
                                                   bytes,
                                                   index,
                                                   options.verification));
                }
            }
        }

        const auto summaries = summarize(measurements);
        std::cout << std::fixed << std::setprecision(3);
        for (const auto& row : summaries) {
            std::cout << "direction=" << row.direction
                      << " bytes=" << row.bytes
                      << " samples=" << row.samples
                      << " median_e2e_ns=" << row.median_e2e_ns
                      << " p05_e2e_ns=" << row.p05_e2e_ns
                      << " p95_e2e_ns=" << row.p95_e2e_ns
                      << " median_destination_allocation_ns="
                      << row.median_destination_ns
                      << " median_kernel_wall_ns=" << row.median_kernel_ns
                      << " e2e_gbps="
                      << gb_per_second(row.bytes, row.median_e2e_ns)
                      << " kernel_wall_gbps="
                      << gb_per_second(row.bytes, row.median_kernel_ns)
                      << '\n';
        }
        write_csv(options.csv_path, measurements);
        write_json(options.json_path, options, summaries);
        std::cout << "benchmark=xrt_migration status=PASS rows="
                  << measurements.size() << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        usage(argv[0]);
        std::cerr << "error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
