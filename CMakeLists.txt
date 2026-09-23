cmake_minimum_required(VERSION 3.16)
project(retention_tiles VERSION 0.8.2 LANGUAGES C CXX)

option(RTMEM_ENABLE_XRT "Build the AMD XRT backend" OFF)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rtmem
    src/rtmem.cpp
    src/rtmem_cpp.cpp
    src/trace_writer.cpp
    src/backend_host.cpp
)
if(RTMEM_ENABLE_XRT)
    find_path(XRT_INCLUDE_DIR xrt/xrt_bo.h
        HINTS ENV XILINX_XRT
        PATH_SUFFIXES include
    )
    find_library(XRT_COREUTIL_LIBRARY xrt_coreutil
        HINTS ENV XILINX_XRT
        PATH_SUFFIXES lib lib64
    )
    if(NOT XRT_INCLUDE_DIR OR NOT XRT_COREUTIL_LIBRARY)
        message(FATAL_ERROR
            "XRT headers or libxrt_coreutil not found; set XILINX_XRT")
    endif()
    target_sources(rtmem PRIVATE src/backend_xrt.cpp)
    target_include_directories(rtmem PRIVATE ${XRT_INCLUDE_DIR})
    target_link_libraries(rtmem PRIVATE ${XRT_COREUTIL_LIBRARY} Threads::Threads)
    target_compile_definitions(rtmem PRIVATE RTMEM_ENABLE_XRT=1)
else()
    target_sources(rtmem PRIVATE src/backend_xrt_stub.cpp)
endif()
target_include_directories(rtmem
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_options(rtmem PRIVATE -Wall -Wextra -Wpedantic)

add_executable(retention_sim
    sim/main.cpp
    sim/retention_model.cpp
    sim/workload_trace.cpp
    sim/device_profile.cpp
    sim/evidence_model.cpp
)
target_include_directories(retention_sim PRIVATE sim)

enable_testing()
add_executable(test_c_api tests/test_c_api.c)
target_link_libraries(test_c_api PRIVATE rtmem)
add_test(NAME c_api COMMAND test_c_api)

add_executable(test_cpp_api tests/test_cpp_api.cpp)
target_link_libraries(test_cpp_api PRIVATE rtmem)
add_test(NAME cpp_api COMMAND test_cpp_api)

add_executable(test_concurrency tests/test_concurrency.cpp)
target_link_libraries(test_concurrency PRIVATE rtmem Threads::Threads)
add_test(NAME concurrency COMMAND test_concurrency)

add_executable(test_trace tests/test_trace.cpp)
target_link_libraries(test_trace PRIVATE rtmem)
set(TEST_WORKLOAD_TRACE ${CMAKE_CURRENT_BINARY_DIR}/test_workload.rttrace)
add_test(NAME trace_api COMMAND test_trace ${TEST_WORKLOAD_TRACE})
set_tests_properties(trace_api PROPERTIES FIXTURES_SETUP workload_trace)
add_test(NAME workload_replay
    COMMAND retention_sim --trace ${TEST_WORKLOAD_TRACE} --policy oracle)
set_tests_properties(workload_replay
    PROPERTIES FIXTURES_REQUIRED workload_trace)
add_test(NAME workload_hint_replay
    COMMAND retention_sim --trace ${TEST_WORKLOAD_TRACE} --policy hint)
set_tests_properties(workload_hint_replay
    PROPERTIES FIXTURES_REQUIRED workload_trace)

add_executable(test_hls_blocked_matmul
    tests/test_hls_blocked_matmul.cpp
    fpga/u280/rtmem_blocked_matmul.cpp)
target_compile_options(test_hls_blocked_matmul PRIVATE
    -Wno-unknown-pragmas -Wno-unused-label)
add_test(NAME hls_blocked_matmul COMMAND test_hls_blocked_matmul)

add_executable(traced_matmul examples/traced_matmul.cpp)
target_link_libraries(traced_matmul PRIVATE rtmem)

add_executable(traced_blocked_matmul examples/traced_blocked_matmul.cpp)
target_link_libraries(traced_blocked_matmul PRIVATE rtmem)

add_executable(traced_tiled_matmul examples/traced_tiled_matmul.cpp)
target_link_libraries(traced_tiled_matmul PRIVATE rtmem)

add_executable(traced_structures examples/traced_structures.cpp)
target_link_libraries(traced_structures PRIVATE rtmem)

add_executable(traced_structures_variable
    examples/traced_structures_variable.cpp)
target_link_libraries(traced_structures_variable PRIVATE rtmem)

foreach(BENCHMARK bfs hash_join stencil)
    set(BENCHMARK_TRACE
        ${CMAKE_CURRENT_BINARY_DIR}/${BENCHMARK}.rttrace)
    add_test(NAME benchmark_${BENCHMARK}
        COMMAND traced_structures_variable ${BENCHMARK} ${BENCHMARK_TRACE})
    set_tests_properties(benchmark_${BENCHMARK}
        PROPERTIES FIXTURES_SETUP trace_${BENCHMARK})
    add_test(NAME benchmark_${BENCHMARK}_hint
        COMMAND retention_sim --trace ${BENCHMARK_TRACE} --policy hint)
    set_tests_properties(benchmark_${BENCHMARK}_hint PROPERTIES
        FIXTURES_REQUIRED trace_${BENCHMARK}
        PASS_REGULAR_EXPRESSION "status=SAFE")
    add_test(NAME benchmark_${BENCHMARK}_oracle
        COMMAND retention_sim --trace ${BENCHMARK_TRACE} --policy oracle)
    set_tests_properties(benchmark_${BENCHMARK}_oracle PROPERTIES
        FIXTURES_REQUIRED trace_${BENCHMARK}
        PASS_REGULAR_EXPRESSION "status=SAFE")
endforeach()

add_test(NAME benchmark_policy_metrics
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark_replay.sh
        $<TARGET_FILE:traced_structures_variable>
        $<TARGET_FILE:retention_sim>
        ${CMAKE_CURRENT_BINARY_DIR})

set(BLOCKED_MATMUL_TRACE
    ${CMAKE_CURRENT_BINARY_DIR}/blocked_matmul.rttrace)
add_test(NAME blocked_matmul_capture
    COMMAND traced_blocked_matmul --trace ${BLOCKED_MATMUL_TRACE})
set_tests_properties(blocked_matmul_capture PROPERTIES
    FIXTURES_SETUP trace_blocked_matmul
    PASS_REGULAR_EXPRESSION "status=PASS")
add_test(NAME blocked_matmul_policy_metrics
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_blocked_matmul.sh
        $<TARGET_FILE:traced_blocked_matmul>
        $<TARGET_FILE:retention_sim>
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/profiles/large_experiment.profile)

add_test(NAME tiled_matmul_reuse
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tiled_matmul.sh
        $<TARGET_FILE:traced_tiled_matmul>
        $<TARGET_FILE:retention_sim>
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/profiles/large_experiment.profile)

add_test(NAME evidence_replay
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_evidence_replay.sh
        $<TARGET_FILE:retention_sim>
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/profiles
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/traces)

add_test(NAME lifetime_analysis
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifetime_analysis.sh
        $<TARGET_FILE:retention_sim>
        $<TARGET_FILE:traced_blocked_matmul>
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/profiles/large_experiment.profile)

if(RTMEM_ENABLE_XRT)
    add_executable(xrt_vertical_slice examples/xrt_vertical_slice.cpp)
    target_link_libraries(xrt_vertical_slice PRIVATE rtmem)

    add_executable(xrt_migration_benchmark
        examples/xrt_migration_benchmark.cpp)
    target_link_libraries(xrt_migration_benchmark PRIVATE rtmem)

    add_executable(xrt_blocked_matmul examples/xrt_blocked_matmul.cpp)
    target_include_directories(xrt_blocked_matmul PRIVATE ${XRT_INCLUDE_DIR})
    target_link_libraries(xrt_blocked_matmul
        PRIVATE ${XRT_COREUTIL_LIBRARY} Threads::Threads)
endif()

install(TARGETS rtmem
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
