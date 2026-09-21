#include "rtmem/rtmem.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rtmem::buffer;
using rtmem::policy;
using rtmem::region;
using rtmem::retention_class;
using rtmem::runtime;
using rtmem::traced_view;

std::uint64_t run_bfs(runtime& runtime) {
    constexpr std::size_t kVertices = 8;
    constexpr std::array<std::uint32_t, kVertices + 1> kOffsets{
        0, 2, 3, 5, 6, 8, 9, 10, 10};
    constexpr std::array<std::uint32_t, 10> kEdges{
        1, 2, 3, 3, 4, 5, 5, 6, 7, 7};

    policy graph_policy(retention_class::durable);
    policy visited_policy(retention_class::epoch);
    policy frontier_policy(retention_class::ephemeral);
    region graph_region(runtime, graph_policy, "bfs_graph");
    region visited_region(runtime, visited_policy, "bfs_visited");
    region frontier_region(runtime, frontier_policy, "bfs_frontiers");

    buffer offsets_buffer(graph_region, kOffsets.size() * sizeof(std::uint32_t),
                          64);
    buffer edges_buffer(graph_region, kEdges.size() * sizeof(std::uint32_t),
                        64);
    buffer visited_buffer(visited_region, kVertices * sizeof(std::uint32_t),
                          64);
    buffer frontier_a_buffer(frontier_region,
                             kVertices * sizeof(std::uint32_t), 64);
    buffer frontier_b_buffer(frontier_region,
                             kVertices * sizeof(std::uint32_t), 64);

    traced_view<std::uint32_t> offsets(offsets_buffer, kOffsets.size());
    traced_view<std::uint32_t> edges(edges_buffer, kEdges.size());
    traced_view<std::uint32_t> visited(visited_buffer, kVertices);
    traced_view<std::uint32_t> frontier_a(frontier_a_buffer, kVertices);
    traced_view<std::uint32_t> frontier_b(frontier_b_buffer, kVertices);

    runtime.trace_phase("bfs_initialize");
    for (std::size_t index = 0; index < kOffsets.size(); ++index) {
        offsets.store(index, kOffsets[index]);
    }
    for (std::size_t index = 0; index < kEdges.size(); ++index) {
        edges.store(index, kEdges[index]);
    }
    for (std::size_t vertex = 0; vertex < kVertices; ++vertex) {
        visited.store(vertex, 0);
    }
    visited.store(0, 1);
    frontier_a.store(0, 0);

    std::size_t current_count = 1;
    bool a_is_current = true;
    std::uint64_t levels = 0;
    while (current_count != 0) {
        runtime.trace_phase("bfs_level_" + std::to_string(levels));
        std::size_t next_count = 0;
        for (std::size_t index = 0; index < current_count; ++index) {
            const auto vertex = a_is_current ? frontier_a.load(index)
                                             : frontier_b.load(index);
            const auto begin = offsets.load(vertex);
            const auto end = offsets.load(vertex + 1);
            for (std::uint32_t edge = begin; edge < end; ++edge) {
                const auto neighbor = edges.load(edge);
                if (visited.load(neighbor) == 0) {
                    visited.store(neighbor, 1);
                    if (a_is_current) {
                        frontier_b.store(next_count, neighbor);
                    } else {
                        frontier_a.store(next_count, neighbor);
                    }
                    ++next_count;
                }
                runtime.trace_compute(2);
            }
        }
        runtime.trace_barrier(levels + 1);
        current_count = next_count;
        a_is_current = !a_is_current;
        ++levels;
    }

    runtime.trace_phase("bfs_verify");
    std::uint64_t visited_count = 0;
    for (std::size_t vertex = 0; vertex < kVertices; ++vertex) {
        visited_count += visited.load(vertex);
    }
    if (visited_count != kVertices || levels != 5) {
        throw std::runtime_error("BFS result mismatch");
    }
    return (levels << 32U) | visited_count;
}

std::size_t hash_slot(std::int32_t key, std::size_t slots) {
    const auto mixed = static_cast<std::uint32_t>(key) * 2654435761U;
    return static_cast<std::size_t>(mixed) & (slots - 1);
}

std::uint64_t run_hash_join(runtime& runtime) {
    constexpr std::array<std::int32_t, 12> kBuildKeys{
        1, 5, 9, 13, 17, 21, 25, 29, 33, 37, 41, 45};
    constexpr std::array<std::int32_t, 12> kProbeKeys{
        45, 2, 17, 99, 1, 41, 13, 6, 29, 77, 9, 33};
    constexpr std::array<std::int32_t, 8> kExpectedMatches{
        45, 17, 1, 41, 13, 29, 9, 33};
    constexpr std::size_t kTableSlots = 32;
    constexpr auto kEmpty = std::numeric_limits<std::int32_t>::min();

    policy input_policy(retention_class::durable);
    policy table_policy(retention_class::epoch);
    policy match_policy(retention_class::ephemeral);
    region input_region(runtime, input_policy, "join_inputs");
    region table_region(runtime, table_policy, "join_hash_table");
    region match_region(runtime, match_policy, "join_matches");

    buffer build_buffer(input_region,
                        kBuildKeys.size() * sizeof(std::int32_t), 64);
    buffer probe_buffer(input_region,
                        kProbeKeys.size() * sizeof(std::int32_t), 64);
    buffer table_buffer(table_region, kTableSlots * sizeof(std::int32_t), 64);
    buffer matches_buffer(match_region,
                          kProbeKeys.size() * sizeof(std::int32_t), 64);

    traced_view<std::int32_t> build(build_buffer, kBuildKeys.size());
    traced_view<std::int32_t> probe(probe_buffer, kProbeKeys.size());
    traced_view<std::int32_t> table(table_buffer, kTableSlots);
    traced_view<std::int32_t> matches(matches_buffer, kProbeKeys.size());

    runtime.trace_phase("join_initialize");
    for (std::size_t index = 0; index < kBuildKeys.size(); ++index) {
        build.store(index, kBuildKeys[index]);
    }
    for (std::size_t index = 0; index < kProbeKeys.size(); ++index) {
        probe.store(index, kProbeKeys[index]);
    }
    for (std::size_t slot = 0; slot < kTableSlots; ++slot) {
        table.store(slot, kEmpty);
    }

    runtime.trace_phase("join_build");
    for (std::size_t index = 0; index < kBuildKeys.size(); ++index) {
        const auto key = build.load(index);
        auto slot = hash_slot(key, kTableSlots);
        while (table.load(slot) != kEmpty) {
            slot = (slot + 1) & (kTableSlots - 1);
            runtime.trace_compute(1);
        }
        table.store(slot, key);
        runtime.trace_compute(2);
    }
    runtime.trace_barrier(1);

    runtime.trace_phase("join_probe");
    std::size_t match_count = 0;
    for (std::size_t index = 0; index < kProbeKeys.size(); ++index) {
        const auto key = probe.load(index);
        auto slot = hash_slot(key, kTableSlots);
        for (std::size_t attempt = 0; attempt < kTableSlots; ++attempt) {
            const auto candidate = table.load(slot);
            runtime.trace_compute(2);
            if (candidate == key) {
                matches.store(match_count++, key);
                break;
            }
            if (candidate == kEmpty) {
                break;
            }
            slot = (slot + 1) & (kTableSlots - 1);
        }
    }
    runtime.trace_barrier(2);

    runtime.trace_phase("join_verify");
    if (match_count != kExpectedMatches.size()) {
        throw std::runtime_error("hash-join match count mismatch");
    }
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < match_count; ++index) {
        const auto actual = matches.load(index);
        if (actual != kExpectedMatches[index]) {
            throw std::runtime_error("hash-join output mismatch");
        }
        sum += static_cast<std::uint32_t>(actual);
    }
    return (static_cast<std::uint64_t>(match_count) << 32U) | sum;
}

std::uint64_t run_stencil(runtime& runtime) {
    constexpr std::size_t kElements = 16;
    constexpr std::size_t kTileElements = 4;
    constexpr std::size_t kIterations = 3;
    constexpr std::array<double, 3> kCoefficients{0.25, 0.5, 0.25};

    policy coefficient_policy(retention_class::durable);
    policy grid_policy(retention_class::epoch);
    policy scratch_policy(retention_class::ephemeral);
    region coefficient_region(runtime,
                              coefficient_policy,
                              "stencil_coefficients");
    region grid_region(runtime, grid_policy, "stencil_grids");
    region scratch_region(runtime, scratch_policy, "stencil_scratch");

    buffer coefficient_buffer(coefficient_region,
                              kCoefficients.size() * sizeof(double), 64);
    buffer grid_a_buffer(grid_region, kElements * sizeof(double), 64);
    buffer grid_b_buffer(grid_region, kElements * sizeof(double), 64);
    buffer scratch_buffer(scratch_region,
                          (kTileElements + 2) * sizeof(double), 64);

    traced_view<double> coefficients(coefficient_buffer,
                                     kCoefficients.size());
    traced_view<double> grid_a(grid_a_buffer, kElements);
    traced_view<double> grid_b(grid_b_buffer, kElements);
    traced_view<double> scratch(scratch_buffer, kTileElements + 2);

    std::array<double, kElements> reference_a{};
    std::array<double, kElements> reference_b{};
    runtime.trace_phase("stencil_initialize");
    for (std::size_t index = 0; index < kCoefficients.size(); ++index) {
        coefficients.store(index, kCoefficients[index]);
    }
    for (std::size_t index = 0; index < kElements; ++index) {
        const auto value = static_cast<double>(index + 1);
        grid_a.store(index, value);
        reference_a[index] = value;
    }

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        runtime.trace_phase("stencil_iteration_" +
                            std::to_string(iteration));
        const bool a_is_input = (iteration % 2) == 0;
        for (std::size_t tile = 0; tile < kElements;
             tile += kTileElements) {
            for (std::size_t local = 0; local < kTileElements + 2; ++local) {
                const auto position = static_cast<std::ptrdiff_t>(tile) +
                                      static_cast<std::ptrdiff_t>(local) - 1;
                double value = 0.0;
                if (position >= 0 &&
                    position < static_cast<std::ptrdiff_t>(kElements)) {
                    const auto index = static_cast<std::size_t>(position);
                    value = a_is_input ? grid_a.load(index)
                                       : grid_b.load(index);
                }
                scratch.store(local, value);
                runtime.trace_compute(1);
            }

            for (std::size_t local = 0; local < kTileElements; ++local) {
                const auto value =
                    coefficients.load(0) * scratch.load(local) +
                    coefficients.load(1) * scratch.load(local + 1) +
                    coefficients.load(2) * scratch.load(local + 2);
                if (a_is_input) {
                    grid_b.store(tile + local, value);
                } else {
                    grid_a.store(tile + local, value);
                }
                runtime.trace_compute(3);
            }
        }

        for (std::size_t index = 0; index < kElements; ++index) {
            const auto left = index == 0 ? 0.0 : reference_a[index - 1];
            const auto center = reference_a[index];
            const auto right = index + 1 == kElements
                                   ? 0.0
                                   : reference_a[index + 1];
            reference_b[index] = kCoefficients[0] * left +
                                 kCoefficients[1] * center +
                                 kCoefficients[2] * right;
        }
        reference_a.swap(reference_b);
        runtime.trace_barrier(iteration + 1);
    }

    runtime.trace_phase("stencil_verify");
    double sum = 0.0;
    const bool a_is_result = (kIterations % 2) == 0;
    for (std::size_t index = 0; index < kElements; ++index) {
        const auto actual = a_is_result ? grid_a.load(index)
                                        : grid_b.load(index);
        if (std::fabs(actual - reference_a[index]) > 1e-12) {
            throw std::runtime_error("stencil result mismatch");
        }
        sum += actual;
    }
    return static_cast<std::uint64_t>(std::llround(sum * 1024.0));
}

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " <bfs|hash_join|stencil> [output.rttrace]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return 2;
    }

    try {
        const std::string benchmark = argv[1];
        runtime runtime;
        std::unique_ptr<rtmem::trace> recorder;
        if (argc == 3) {
            recorder = std::make_unique<rtmem::trace>(argv[2]);
            runtime.attach_trace(*recorder);
        }

        runtime.trace_phase(benchmark + "_begin");
        std::uint64_t checksum = 0;
        if (benchmark == "bfs") {
            checksum = run_bfs(runtime);
        } else if (benchmark == "hash_join") {
            checksum = run_hash_join(runtime);
        } else if (benchmark == "stencil") {
            checksum = run_stencil(runtime);
        } else {
            usage(argv[0]);
            return 2;
        }
        runtime.trace_phase(benchmark + "_complete");

        if (recorder != nullptr) {
            runtime.detach_trace();
            recorder->flush();
            std::cout << "trace=" << argv[2] << '\n';
        }
        std::cout << "benchmark=" << benchmark << " checksum=" << checksum
                  << " status=PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
