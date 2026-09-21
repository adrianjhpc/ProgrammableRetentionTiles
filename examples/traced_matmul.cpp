#include "rtmem/rtmem.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

constexpr std::size_t kDimension = 64;
constexpr std::size_t kElements = kDimension * kDimension;

std::size_t element(std::size_t row, std::size_t column) {
    return row * kDimension + column;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: " << argv[0] << " [output.rttrace]\n";
        return EXIT_FAILURE;
    }

    try {
        rtmem::runtime runtime;
        std::unique_ptr<rtmem::trace> recorder;
        if (argc == 2) {
            recorder = std::make_unique<rtmem::trace>(argv[1]);
            runtime.attach_trace(*recorder);
        }

        double checksum = 0.0;
        {
            rtmem::policy policy(rtmem::retention_class::ephemeral);
            rtmem::region region(runtime, policy, "traced_matmul");
            rtmem::buffer a(region, kElements * sizeof(double), 64);
            rtmem::buffer b(region, kElements * sizeof(double), 64);
            rtmem::buffer c(region, kElements * sizeof(double), 64);

            {
                rtmem::traced_view<double> a_view(a, kElements);
                rtmem::traced_view<double> b_view(b, kElements);
                rtmem::traced_view<double> c_view(c, kElements);

                runtime.trace_phase("initialize");
                for (std::size_t row = 0; row < kDimension; ++row) {
                    for (std::size_t column = 0; column < kDimension;
                         ++column) {
                        a_view.store(element(row, column),
                                     static_cast<double>(row + column + 1));
                        b_view.store(element(row, column),
                                     row == column ? 1.0 : 0.0);
                        c_view.store(element(row, column), 0.0);
                    }
                }

                runtime.trace_phase("matrix_multiply");
                for (std::size_t row = 0; row < kDimension; ++row) {
                    for (std::size_t column = 0; column < kDimension;
                         ++column) {
                        double sum = 0.0;
                        for (std::size_t inner = 0; inner < kDimension;
                             ++inner) {
                            sum += a_view.load(element(row, inner)) *
                                   b_view.load(element(inner, column));
                            runtime.trace_compute(1);
                        }
                        c_view.store(element(row, column), sum);
                    }
                }
                runtime.trace_barrier(1);

                runtime.trace_phase("verify");
                for (std::size_t index = 0; index < kElements; ++index) {
                    const auto actual = c_view.load(index);
                    const auto expected =
                        static_cast<double>(index / kDimension +
                                            index % kDimension + 1);
                    if (std::fabs(actual - expected) > 1e-12) {
                        throw std::runtime_error("matrix result mismatch");
                    }
                    checksum += actual;
                }

                c_view.close();
                b_view.close();
                a_view.close();
            }
            runtime.trace_phase("complete");
        }

        if (recorder != nullptr) {
            runtime.detach_trace();
            recorder->flush();
            std::cout << "trace=" << argv[1] << '\n';
        }
        std::cout << "matrix_dimension=" << kDimension
                  << " checksum=" << checksum << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
