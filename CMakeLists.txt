cmake_minimum_required(VERSION 3.16)
project(retention_tiles VERSION 0.3.0 LANGUAGES C CXX)

option(RTMEM_ENABLE_XRT "Build the AMD XRT backend" OFF)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rtmem
    src/rtmem.cpp
    src/rtmem_cpp.cpp
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

if(RTMEM_ENABLE_XRT)
    add_executable(xrt_vertical_slice examples/xrt_vertical_slice.cpp)
    target_link_libraries(xrt_vertical_slice PRIVATE rtmem)
endif()

install(TARGETS rtmem
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
