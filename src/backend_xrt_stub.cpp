#include "backend.hpp"

#include <stdexcept>

namespace rtmem::internal {

std::unique_ptr<backend> make_xrt_backend(const rt_xrt_options&) {
    throw std::runtime_error(
        "XRT backend was not compiled; rebuild with RTMEM_ENABLE_XRT=1");
}

}  // namespace rtmem::internal

