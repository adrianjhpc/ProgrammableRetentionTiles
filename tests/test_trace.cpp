#include "rtmem/rtmem.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    assert(argc <= 2);
    const std::string path =
        argc == 2 ? argv[1] : "test_workload.rttrace";
    rtmem::runtime runtime;
    rtmem::trace recorder(path);
    runtime.attach_trace(recorder);

    {
        rtmem::policy policy(rtmem::retention_class::ephemeral);
        rtmem::region region(runtime, policy, "trace_test");
        rtmem::buffer buffer(region, 256, 64);
        rtmem::traced_view<std::uint32_t> view(buffer, 64);
        runtime.trace_phase("write_phase");
        view.store(3, 0x12345678u);
        runtime.trace_compute(10, 7);
        assert(view.load(3) == 0x12345678u);
        runtime.trace_barrier(42, 7);
        view.close();
        buffer.promote(rtmem::retention_class::epoch);
    }

    runtime.detach_trace();
    recorder.flush();

    std::ifstream input(path);
    assert(input);
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    assert(contents.find("RTTRACE 2\n") == 0);
    assert(contents.find("ALLOC ") != std::string::npos);
    assert(contents.find("ACCESS ") != std::string::npos);
    assert(contents.find("COMPUTE ") != std::string::npos);
    assert(contents.find("BARRIER ") != std::string::npos);
    assert(contents.find("HINT ") != std::string::npos);
    assert(contents.find("FREE ") != std::string::npos);
    return 0;
}
