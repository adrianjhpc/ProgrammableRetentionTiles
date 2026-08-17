#include "rtmem/rtmem.hpp"

#include <iostream>
#include <memory_resource>
#include <numeric>
#include <vector>

int main() {
    rtmem::runtime runtime;
    rtmem::policy policy{rtmem::retention_class::ephemeral};
    policy.expiry(rtmem::expiry_action::promote,
                  rtmem::retention_class::epoch);
    rtmem::region iteration{runtime, policy, "iteration"};
    rtmem::retention_resource memory{iteration};

    std::pmr::vector<int> frontier{&memory};
    frontier.resize(1024);
    std::iota(frontier.begin(), frontier.end(), 0);
    std::cout << "frontier sum: "
              << std::accumulate(frontier.begin(), frontier.end(), 0LL)
              << '\n';
}

