#include "rtmem/rtmem.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kDefaultDimension = 16;
constexpr std::size_t kDefaultTile = 4;
constexpr std::size_t kPanelWidth = 4;

struct Options {
    std::string trace_path;
    std::size_t dimension = kDefaultDimension;
    std::size_t tile = kDefaultTile;
};

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " [--trace FILE] [--dimension N] [--tile N]\n";
}

std::size_t parse_size(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() || value == 0 ||
        value > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return static_cast<std::size_t>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--trace" && index + 1 < argc) {
            options.trace_path = argv[++index];
        } else if (argument == "--dimension" && index + 1 < argc) {
            options.dimension = parse_size(argv[++index], "dimension");
        } else if (argument == "--tile" && index + 1 < argc) {
            options.tile = parse_size(argv[++index], "tile size");
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " +
                                        argument);
        }
    }

    if (options.dimension % options.tile != 0 ||
        options.dimension % kPanelWidth != 0) {
        throw std::invalid_argument(
            "dimension must be a multiple of the tile and panel widths");
    }
    return options;
}

std::size_t matrix_elements(std::size_t dimension) {
    if (dimension >
        std::numeric_limits<std::size_t>::max() / dimension) {
        throw std::overflow_error("matrix element count overflow");
    }
    return dimension * dimension;
}

std::size_t element(std::size_t row,
                    std::size_t column,
                    std::size_t row_length) {
    return row * row_length + column;
}

double a_value(std::size_t row, std::size_t column) {
    const auto value = static_cast<int>((row * 3 + column * 5) % 17) - 8;
    return static_cast<double>(value) * 0.25;
}

double b_value(std::size_t row, std::size_t column) {
    const auto value = static_cast<int>((row * 7 + column * 11) % 19) - 9;
    return static_cast<double>(value) * 0.125;
}

double run_blocked_matmul(rtmem::runtime& runtime,
                          std::size_t dimension,
                          std::size_t tile) {
    const auto elements = matrix_elements(dimension);
    if (elements > std::numeric_limits<std::size_t>::max() / sizeof(double)) {
        throw std::overflow_error("matrix byte count overflow");
    }
    const auto matrix_bytes = elements * sizeof(double);
    const auto tile_elements = matrix_elements(tile);

    rtmem::policy durable(rtmem::retention_class::durable);
    rtmem::policy epoch(rtmem::retention_class::epoch);
    rtmem::policy ephemeral(rtmem::retention_class::ephemeral);

    rtmem::region input_region(runtime, durable, "matmul_inputs");
    rtmem::region output_region(runtime, durable, "matmul_output");
    rtmem::region accumulator_region(runtime,
                                     epoch,
                                     "matmul_accumulator_tile");
    rtmem::region a_panel_region(runtime, ephemeral, "matmul_a_panel");
    rtmem::region b_panel_region(runtime, ephemeral, "matmul_b_panel");

    rtmem::buffer a_buffer(input_region, matrix_bytes, 64);
    rtmem::buffer b_buffer(input_region, matrix_bytes, 64);
    rtmem::buffer c_buffer(output_region, matrix_bytes, 64);
    rtmem::buffer accumulator_buffer(accumulator_region,
                                     tile_elements * sizeof(double),
                                     64);
    rtmem::buffer a_panel_buffer(a_panel_region,
                                 kPanelWidth * sizeof(double),
                                 64);
    rtmem::buffer b_panel_buffer(b_panel_region,
                                 kPanelWidth * sizeof(double),
                                 64);

    rtmem::traced_view<double> a(a_buffer, elements);
    rtmem::traced_view<double> b(b_buffer, elements);
    rtmem::traced_view<double> c(c_buffer, elements);
    rtmem::traced_view<double> accumulator(accumulator_buffer,
                                           tile_elements);
    rtmem::traced_view<double> a_panel(a_panel_buffer, kPanelWidth);
    rtmem::traced_view<double> b_panel(b_panel_buffer, kPanelWidth);

    std::vector<double> reference_a(elements);
    std::vector<double> reference_b(elements);
    std::vector<double> reference_c(elements, 0.0);

    runtime.trace_phase("blocked_matmul_initialize");
    for (std::size_t row = 0; row < dimension; ++row) {
        for (std::size_t column = 0; column < dimension; ++column) {
            const auto index = element(row, column, dimension);
            reference_a[index] = a_value(row, column);
            reference_b[index] = b_value(row, column);
            a.store(index, reference_a[index]);
            b.store(index, reference_b[index]);
        }
    }

    // Compute a host-only reference. These accesses intentionally do not enter
    // the workload trace; they validate the real blocked kernel below.
    for (std::size_t row = 0; row < dimension; ++row) {
        for (std::size_t column = 0; column < dimension; ++column) {
            double sum = 0.0;
            for (std::size_t inner = 0; inner < dimension; ++inner) {
                sum += reference_a[element(row, inner, dimension)] *
                       reference_b[element(inner, column, dimension)];
            }
            reference_c[element(row, column, dimension)] = sum;
        }
    }

    std::uint64_t barrier = 1;
    for (std::size_t row_block = 0; row_block < dimension;
         row_block += tile) {
        for (std::size_t column_block = 0; column_block < dimension;
             column_block += tile) {
            runtime.trace_phase("blocked_matmul_output_tile");

            // This block persists only while all K panels contributing to one
            // output tile are accumulated.
            for (std::size_t index = 0; index < tile_elements; ++index) {
                accumulator.store(index, 0.0);
            }

            for (std::size_t inner_block = 0; inner_block < dimension;
                 inner_block += kPanelWidth) {
                for (std::size_t local_row = 0; local_row < tile;
                     ++local_row) {
                    for (std::size_t local_column = 0;
                         local_column < tile;
                         ++local_column) {
                        // Repack a short row and column panel for one partial
                        // dot product. This deliberately bounds the panels'
                        // live range below the EPHEMERAL retention window.
                        for (std::size_t local_inner = 0;
                             local_inner < kPanelWidth;
                             ++local_inner) {
                            a_panel.store(
                                local_inner,
                                a.load(element(row_block + local_row,
                                               inner_block + local_inner,
                                               dimension)));
                            b_panel.store(
                                local_inner,
                                b.load(element(inner_block + local_inner,
                                               column_block + local_column,
                                               dimension)));
                        }

                        const auto accumulator_index =
                            element(local_row, local_column, tile);
                        double sum = accumulator.load(accumulator_index);
                        for (std::size_t local_inner = 0;
                             local_inner < kPanelWidth;
                             ++local_inner) {
                            sum += a_panel.load(local_inner) *
                                   b_panel.load(local_inner);
                            runtime.trace_compute(2);
                        }
                        accumulator.store(accumulator_index, sum);
                    }
                }
            }

            // Once complete, drain the epoch tile immediately to durable
            // output. The accumulator can then be overwritten for the next
            // output tile without migration.
            for (std::size_t local_row = 0; local_row < tile; ++local_row) {
                for (std::size_t local_column = 0; local_column < tile;
                     ++local_column) {
                    const auto value = accumulator.load(
                        element(local_row, local_column, tile));
                    c.store(element(row_block + local_row,
                                    column_block + local_column,
                                    dimension),
                            value);
                    runtime.trace_compute(1);
                }
            }
            runtime.trace_barrier(barrier++);
        }
    }

    runtime.trace_phase("blocked_matmul_verify");
    double checksum = 0.0;
    for (std::size_t index = 0; index < elements; ++index) {
        const auto actual = c.load(index);
        const auto expected = reference_c[index];
        const auto tolerance = 1e-12 * (1.0 + std::fabs(expected));
        if (std::fabs(actual - expected) > tolerance) {
            throw std::runtime_error("blocked matrix result mismatch at " +
                                     std::to_string(index));
        }
        checksum += actual;
    }

    b_panel.close();
    a_panel.close();
    accumulator.close();
    c.close();
    b.close();
    a.close();
    return checksum;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        rtmem::runtime runtime;
        std::unique_ptr<rtmem::trace> recorder;
        if (!options.trace_path.empty()) {
            recorder = std::make_unique<rtmem::trace>(options.trace_path);
            runtime.attach_trace(*recorder);
        }

        const auto checksum = run_blocked_matmul(runtime,
                                                 options.dimension,
                                                 options.tile);
        runtime.trace_phase("blocked_matmul_complete");

        if (recorder != nullptr) {
            runtime.detach_trace();
            recorder->flush();
            std::cout << "trace=" << options.trace_path << '\n';
        }
        std::cout << "benchmark=blocked_matmul"
                  << " dimension=" << options.dimension
                  << " tile=" << options.tile
                  << " panel_width=" << kPanelWidth
                  << " checksum=" << checksum
                  << " status=PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << '\n';
        usage(argv[0]);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

