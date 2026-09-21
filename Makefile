CXX ?= g++
CC ?= cc
AR ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude
LDLIBS ?=
RTMEM_ENABLE_XRT ?= 0
XRT_ROOT ?= /home/nx08/shared/fpga/xrt/2.14.384-boost-1.65
XRT_LIB_DIR ?= $(XRT_ROOT)/lib

BUILD_DIR := build
SIM := $(BUILD_DIR)/retention_sim
SIM_SOURCES := sim/main.cpp sim/retention_model.cpp sim/workload_trace.cpp \
	sim/device_profile.cpp
LIB_OBJECTS := $(BUILD_DIR)/rtmem.o $(BUILD_DIR)/rtmem_cpp.o \
	$(BUILD_DIR)/trace_writer.o \
	$(BUILD_DIR)/backend_host.o

ifeq ($(RTMEM_ENABLE_XRT),1)
CPPFLAGS += -DRTMEM_ENABLE_XRT=1 -I$(XRT_ROOT)/include
LDLIBS += -L$(XRT_LIB_DIR) -lxrt_coreutil -pthread
LIB_OBJECTS += $(BUILD_DIR)/backend_xrt.o
else
LIB_OBJECTS += $(BUILD_DIR)/backend_xrt_stub.o
endif
STATIC_LIB := $(BUILD_DIR)/librtmem.a
SHARED_LIB := $(BUILD_DIR)/librtmem.so
C_API_TEST := $(BUILD_DIR)/test_c_api
CPP_API_TEST := $(BUILD_DIR)/test_cpp_api
CONCURRENCY_TEST := $(BUILD_DIR)/test_concurrency
TRACE_API_TEST := $(BUILD_DIR)/test_trace
C_EXAMPLE := $(BUILD_DIR)/c_regions
CPP_EXAMPLE := $(BUILD_DIR)/cpp_pmr
XRT_EXAMPLE := $(BUILD_DIR)/xrt_vertical_slice
TRACE_EXAMPLE := $(BUILD_DIR)/traced_matmul
TRACE_EXAMPLE_OUTPUT := $(BUILD_DIR)/traced_matmul.rttrace
STRUCTURE_BENCH := $(BUILD_DIR)/traced_structures
BFS_TRACE := $(BUILD_DIR)/bfs.rttrace
HASH_JOIN_TRACE := $(BUILD_DIR)/hash_join.rttrace
STENCIL_TRACE := $(BUILD_DIR)/stencil.rttrace
BENCHMARK_TRACES := $(BFS_TRACE) $(HASH_JOIN_TRACE) $(STENCIL_TRACE)

.PHONY: all test examples trace-example benchmarks xrt-example clean

all: $(SIM) $(STATIC_LIB) $(SHARED_LIB)

$(SIM): $(SIM_SOURCES) sim/retention_model.hpp sim/workload_trace.hpp sim/device_profile.hpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isim $(SIM_SOURCES) -o $(SIM)

$(BUILD_DIR)/rtmem.o: src/rtmem.cpp src/backend.hpp src/trace_writer.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/rtmem_cpp.o: src/rtmem_cpp.cpp include/rtmem/rtmem.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/trace_writer.o: src/trace_writer.cpp src/trace_writer.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/backend_host.o: src/backend_host.cpp src/backend.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/backend_xrt_stub.o: src/backend_xrt_stub.cpp src/backend.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/backend_xrt.o: src/backend_xrt.cpp src/backend.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJECTS)
	$(RM) $@
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJECTS)
	$(CXX) -shared $^ $(LDLIBS) -o $@

$(BUILD_DIR)/test_c_api.o: tests/test_c_api.c include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(C_API_TEST): $(BUILD_DIR)/test_c_api.o $(STATIC_LIB)
	$(CXX) $^ $(LDLIBS) -o $@

$(CPP_API_TEST): tests/test_cpp_api.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(CONCURRENCY_TEST): tests/test_concurrency.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -pthread $^ $(LDLIBS) -o $@

$(TRACE_API_TEST): tests/test_trace.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(C_EXAMPLE): examples/c_regions.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c examples/c_regions.c -o $(BUILD_DIR)/c_regions.o
	$(CXX) $(BUILD_DIR)/c_regions.o $(STATIC_LIB) $(LDLIBS) -o $@

$(CPP_EXAMPLE): examples/cpp_pmr.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(TRACE_EXAMPLE): examples/traced_matmul.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(STRUCTURE_BENCH): examples/traced_structures.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BFS_TRACE): $(STRUCTURE_BENCH)
	$(STRUCTURE_BENCH) bfs $@

$(HASH_JOIN_TRACE): $(STRUCTURE_BENCH)
	$(STRUCTURE_BENCH) hash_join $@

$(STENCIL_TRACE): $(STRUCTURE_BENCH)
	$(STRUCTURE_BENCH) stencil $@

$(XRT_EXAMPLE): examples/xrt_vertical_slice.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

test: $(SIM) $(C_API_TEST) $(CPP_API_TEST) $(CONCURRENCY_TEST) $(TRACE_API_TEST) $(BENCHMARK_TRACES)
	$(SIM) --self-test
	$(SIM) --trace traces/smoke.trace --profile profiles/default.profile
	$(C_API_TEST)
	$(CPP_API_TEST)
	$(CONCURRENCY_TEST)
	$(TRACE_API_TEST) $(BUILD_DIR)/test_workload.rttrace
	$(SIM) --trace $(BUILD_DIR)/test_workload.rttrace --policy hint
	$(SIM) --trace $(BUILD_DIR)/test_workload.rttrace --policy oracle
	$(SIM) --trace $(BFS_TRACE) --policy hint
	$(SIM) --trace $(HASH_JOIN_TRACE) --policy hint
	$(SIM) --trace $(STENCIL_TRACE) --policy hint
	$(SIM) --trace $(STENCIL_TRACE) --policy oracle
	sh tests/test_benchmark_replay.sh $(STRUCTURE_BENCH) $(SIM) $(BUILD_DIR)

examples: $(C_EXAMPLE) $(CPP_EXAMPLE) $(TRACE_EXAMPLE) $(STRUCTURE_BENCH)
	$(C_EXAMPLE)
	$(CPP_EXAMPLE)
	$(TRACE_EXAMPLE)
	$(STRUCTURE_BENCH) bfs
	$(STRUCTURE_BENCH) hash_join
	$(STRUCTURE_BENCH) stencil

trace-example: $(SIM) $(TRACE_EXAMPLE)
	$(TRACE_EXAMPLE) $(TRACE_EXAMPLE_OUTPUT)
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy hint
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy oracle
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy durable

benchmarks: $(SIM) $(BENCHMARK_TRACES)
	@set -e; \
	for trace in $(BENCHMARK_TRACES); do \
		for policy in hint ephemeral epoch durable oracle; do \
			echo "=== $$trace / $$policy ==="; \
			$(SIM) --trace $$trace --policy $$policy; \
		done; \
	done

ifeq ($(RTMEM_ENABLE_XRT),1)
xrt-example: $(XRT_EXAMPLE)
else
xrt-example:
	@echo "Rebuild with RTMEM_ENABLE_XRT=1 to enable the XRT example"
	@exit 1
endif

clean:
	rm -rf $(BUILD_DIR)
