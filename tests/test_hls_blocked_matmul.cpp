#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

extern "C" void rtmem_blocked_matmul(const double* durable_a,
                                      const double* durable_b,
                                      double* durable_c,
                                      double* ephemeral_panels,
                                      double* epoch_accumulator,
                                      std::uint32_t dimension,
                                      std::uint32_t tile);

int main() {
    constexpr std::uint32_t dimension = 8;
    constexpr std::uint32_t tile = 4;
    std::vector<double> a(dimension * dimension);
    std::vector<double> b(dimension * dimension);
    std::vector<double> c(dimension * dimension, 0.0);
    std::vector<double> panels(2 * tile * tile, 0.0);
    std::vector<double> accumulator(tile * tile, 0.0);

    for (std::uint32_t row = 0; row < dimension; ++row) {
        for (std::uint32_t column = 0; column < dimension; ++column) {
            const auto index = row * dimension + column;
            a[index] = static_cast<double>((row + 2 * column) % 7) * 0.25;
            b[index] = static_cast<double>((3 * row + column) % 11) * 0.125;
        }
    }

    rtmem_blocked_matmul(a.data(),
                         b.data(),
                         c.data(),
                         panels.data(),
                         accumulator.data(),
                         dimension,
                         tile);

    for (std::uint32_t row = 0; row < dimension; ++row) {
        for (std::uint32_t column = 0; column < dimension; ++column) {
            double expected = 0.0;
            for (std::uint32_t inner = 0; inner < dimension; ++inner) {
                expected += a[row * dimension + inner] *
                            b[inner * dimension + column];
            }
            assert(std::fabs(c[row * dimension + column] - expected) < 1e-12);
        }
    }
    return 0;
}
