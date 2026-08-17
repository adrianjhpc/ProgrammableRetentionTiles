cmake_minimum_required(VERSION 3.16)
project(retention_tiles VERSION 0.2.0 LANGUAGES C CXX)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(rtmem
    src/rtmem.cpp
    src/rtmem_cpp.cpp
)
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

install(TARGETS rtmem
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
