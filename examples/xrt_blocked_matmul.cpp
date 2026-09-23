#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_kernel.h>

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
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct Options {
    std::string xclbin;
    std::string device = "0";
    std::uint32_t dimension = 256;
    std::uint32_t tile = 16;
    std::size_t warmup = 1;
    std::size_t iterations = 5;
    std::string verification = "sample";
    std::string csv_path;
    std::string json_path;
};

std::uint64_t elapsed_ns(clock_type::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_type::now() - start)
            .count());
}

std::uint64_t parse_number(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return value;
}

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " XCLBIN [DEVICE] [--dimension N] [--tile N]"
                 " [--warmup N] [--iterations N]"
                 " [--verify full|sample] [--csv FILE] [--json FILE]\n";
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
        if (argument == "--dimension" && index < argc) {
            const auto value = parse_number(argv[index++], "dimension");
            if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("dimension is out of range");
            }
            options.dimension = static_cast<std::uint32_t>(value);
        } else if (argument == "--tile" && index < argc) {
            const auto value = parse_number(argv[index++], "tile");
            if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("tile is out of range");
            }
            options.tile = static_cast<std::uint32_t>(value);
        } else if (argument == "--warmup" && index < argc) {
            options.warmup = static_cast<std::size_t>(
                parse_number(argv[index++], "warmup count"));
        } else if (argument == "--iterations" && index < argc) {
            options.iterations = static_cast<std::size_t>(
                parse_number(argv[index++], "iteration count"));
        } else if (argument == "--verify" && index < argc) {
            options.verification = argv[index++];
            if (options.verification != "full" &&
                options.verification != "sample") {
                throw std::invalid_argument("verification must be full or sample");
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
    if (options.iterations == 0 || options.dimension % options.tile != 0) {
        throw std::invalid_argument(
            "iterations must be nonzero and dimension divisible by tile");
    }
    return options;
}

xrt::device open_device(const std::string& selected) {
    if (selected.find(':') != std::string::npos) {
        return xrt::device(selected);
    }
    const auto index = parse_number(selected, "device index");
    if (index > std::numeric_limits<unsigned int>::max()) {
        throw std::invalid_argument("device index is out of range");
    }
    return xrt::device(static_cast<unsigned int>(index));
}

double a_value(std::uint32_t row, std::uint32_t column) {
    const auto value = static_cast<int>((row * 3 + column * 5) % 17) - 8;
    return static_cast<double>(value) * 0.25;
}

double b_value(std::uint32_t row, std::uint32_t column) {
    const auto value = static_cast<int>((row * 7 + column * 11) % 19) - 9;
    return static_cast<double>(value) * 0.125;
}

double expected_element(std::uint32_t row,
                        std::uint32_t column,
                        std::uint32_t dimension) {
    double result = 0.0;
    for (std::uint32_t inner = 0; inner < dimension; ++inner) {
        result += a_value(row, inner) * b_value(inner, column);
    }
    return result;
}

void verify(const double* actual, const Options& options) {
    const auto check = [&](std::uint32_t row, std::uint32_t column) {
        const auto expected = expected_element(row, column, options.dimension);
        const auto value = actual[static_cast<std::size_t>(row) *
                                      options.dimension +
                                  column];
        const auto tolerance = 1e-9 * (1.0 + std::fabs(expected));
        if (std::fabs(value - expected) > tolerance) {
            throw std::runtime_error(
                "matrix mismatch at (" + std::to_string(row) + "," +
                std::to_string(column) + "): expected " +
                std::to_string(expected) + ", got " +
                std::to_string(value));
        }
    };
    if (options.verification == "full") {
        for (std::uint32_t row = 0; row < options.dimension; ++row) {
            for (std::uint32_t column = 0; column < options.dimension;
                 ++column) {
                check(row, column);
            }
        }
        return;
    }
    const std::uint32_t points = std::min<std::uint32_t>(32, options.dimension);
    for (std::uint32_t index = 0; index < points; ++index) {
        const auto row = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(index) * options.dimension) / points);
        const auto column = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(index) * 2654435761ULL) %
            options.dimension);
        check(row, column);
    }
    check(options.dimension - 1, options.dimension - 1);
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto position = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())));
    return values.at(std::max<std::size_t>(1, position) - 1);
}

std::string json_escape(const std::string& text) {
    std::string result;
    for (const char character : text) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto elements = static_cast<std::size_t>(options.dimension) *
                              options.dimension;
        if (elements > std::numeric_limits<std::size_t>::max() /
                           sizeof(double)) {
            throw std::overflow_error("matrix size overflow");
        }
        const auto matrix_bytes = elements * sizeof(double);
        const auto tile_elements = static_cast<std::size_t>(options.tile) *
                                   options.tile;

        auto device = open_device(options.device);
        const auto uuid = device.load_xclbin(options.xclbin);
        xrt::kernel kernel(device, uuid, "rtmem_blocked_matmul");

        xrt::bo a(device, matrix_bytes, xrt::bo::flags::normal,
                  kernel.group_id(0));
        xrt::bo b(device, matrix_bytes, xrt::bo::flags::normal,
                  kernel.group_id(1));
        xrt::bo c(device, matrix_bytes, xrt::bo::flags::normal,
                  kernel.group_id(2));
        xrt::bo panels(device,
                       2 * tile_elements * sizeof(double),
                       xrt::bo::flags::device_only,
                       kernel.group_id(3));
        xrt::bo accumulator(device,
                            tile_elements * sizeof(double),
                            xrt::bo::flags::device_only,
                            kernel.group_id(4));

        auto* a_data = a.map<double*>();
        auto* b_data = b.map<double*>();
        auto* c_data = c.map<double*>();
        for (std::uint32_t row = 0; row < options.dimension; ++row) {
            for (std::uint32_t column = 0; column < options.dimension;
                 ++column) {
                const auto index = static_cast<std::size_t>(row) *
                                       options.dimension +
                                   column;
                a_data[index] = a_value(row, column);
                b_data[index] = b_value(row, column);
                c_data[index] = 0.0;
            }
        }
        const auto upload_start = clock_type::now();
        a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        b.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        c.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        const auto initialization_upload_ns = elapsed_ns(upload_start);

        const auto execute = [&]() {
            const auto start = clock_type::now();
            auto run = kernel(a,
                              b,
                              c,
                              panels,
                              accumulator,
                              options.dimension,
                              options.tile);
            run.wait();
            return elapsed_ns(start);
        };
        for (std::size_t index = 0; index < options.warmup; ++index) {
            (void)execute();
        }
        std::vector<std::uint64_t> samples;
        for (std::size_t index = 0; index < options.iterations; ++index) {
            samples.push_back(execute());
        }

        const auto download_start = clock_type::now();
        c.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        const auto result_download_ns = elapsed_ns(download_start);
        verify(c_data, options);

        const auto median = percentile(samples, 0.50);
        const auto p05 = percentile(samples, 0.05);
        const auto p95 = percentile(samples, 0.95);
        const auto operations = 2.0 * options.dimension * options.dimension *
                                static_cast<double>(options.dimension);
        const auto gflops = operations / static_cast<double>(median);

        if (!options.csv_path.empty()) {
            std::ofstream csv(options.csv_path,
                              std::ios::out | std::ios::trunc);
            if (!csv) {
                throw std::runtime_error("cannot open CSV output");
            }
            csv << "iteration,dimension,tile,kernel_wall_ns,gflops\n";
            csv << std::fixed << std::setprecision(6);
            for (std::size_t index = 0; index < samples.size(); ++index) {
                csv << index << ',' << options.dimension << ',' << options.tile
                    << ',' << samples[index] << ','
                    << operations / static_cast<double>(samples[index])
                    << '\n';
            }
        }
        if (!options.json_path.empty()) {
            std::ofstream json(options.json_path,
                               std::ios::out | std::ios::trunc);
            if (!json) {
                throw std::runtime_error("cannot open JSON output");
            }
            json << std::fixed << std::setprecision(6)
                 << "{\n  \"schema\": \"RTMEM_XRT_BLOCKED_MATMUL_1\",\n"
                 << "  \"xclbin\": \"" << json_escape(options.xclbin)
                 << "\",\n  \"device\": \"" << json_escape(options.device)
                 << "\",\n  \"dimension\": " << options.dimension
                 << ",\n  \"tile\": " << options.tile
                 << ",\n  \"warmup\": " << options.warmup
                 << ",\n  \"iterations\": " << options.iterations
                 << ",\n  \"verification\": \"" << options.verification
                 << "\",\n  \"initialization_upload_ns\": "
                 << initialization_upload_ns
                 << ",\n  \"result_download_ns\": " << result_download_ns
                 << ",\n  \"median_kernel_wall_ns\": " << median
                 << ",\n  \"p05_kernel_wall_ns\": " << p05
                 << ",\n  \"p95_kernel_wall_ns\": " << p95
                 << ",\n  \"median_gflops\": " << gflops
                 << ",\n  \"status\": \"PASS\"\n}\n";
        }

        std::cout << std::fixed << std::setprecision(3)
                  << "benchmark=xrt_blocked_matmul"
                  << " dimension=" << options.dimension
                  << " tile=" << options.tile
                  << " iterations=" << options.iterations
                  << " median_kernel_wall_ns=" << median
                  << " p05_kernel_wall_ns=" << p05
                  << " p95_kernel_wall_ns=" << p95
                  << " median_gflops=" << gflops
                  << " initialization_upload_ns=" << initialization_upload_ns
                  << " result_download_ns=" << result_download_ns
                  << " status=PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        usage(argv[0]);
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
