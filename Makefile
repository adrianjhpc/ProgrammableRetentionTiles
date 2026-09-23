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
	sim/device_profile.cpp sim/evidence_model.cpp
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
HLS_MATMUL_TEST := $(BUILD_DIR)/test_hls_blocked_matmul
C_EXAMPLE := $(BUILD_DIR)/c_regions
CPP_EXAMPLE := $(BUILD_DIR)/cpp_pmr
XRT_EXAMPLE := $(BUILD_DIR)/xrt_vertical_slice
XRT_MIGRATION_BENCH := $(BUILD_DIR)/xrt_migration_benchmark
XRT_BLOCKED_MATMUL := $(BUILD_DIR)/xrt_blocked_matmul
TRACE_EXAMPLE := $(BUILD_DIR)/traced_matmul
TRACE_EXAMPLE_OUTPUT := $(BUILD_DIR)/traced_matmul.rttrace
BLOCKED_MATMUL := $(BUILD_DIR)/traced_blocked_matmul
BLOCKED_MATMUL_TRACE := $(BUILD_DIR)/blocked_matmul.rttrace
TILED_MATMUL := $(BUILD_DIR)/traced_tiled_matmul
TILED_MATMUL_TRACE := $(BUILD_DIR)/tiled_matmul.rttrace
STRUCTURE_BENCH := $(BUILD_DIR)/traced_structures
STRUCTURE_VARIABLE_BENCH := $(BUILD_DIR)/traced_structures_variable
BFS_TRACE := $(BUILD_DIR)/bfs.rttrace
HASH_JOIN_TRACE := $(BUILD_DIR)/hash_join.rttrace
STENCIL_TRACE := $(BUILD_DIR)/stencil.rttrace
BENCHMARK_TRACES := $(BFS_TRACE) $(HASH_JOIN_TRACE) $(STENCIL_TRACE)

.PHONY: all test examples trace-example blocked-matmul tiled-matmul tiled-experiments benchmarks xrt-example xrt-experiments clean

all: $(SIM) $(STATIC_LIB) $(SHARED_LIB)

$(SIM): $(SIM_SOURCES) sim/retention_model.hpp sim/workload_trace.hpp sim/device_profile.hpp sim/evidence_model.hpp
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

$(HLS_MATMUL_TEST): tests/test_hls_blocked_matmul.cpp fpga/u280/rtmem_blocked_matmul.cpp
	$(CXX) $(CXXFLAGS) -Wno-unknown-pragmas -Wno-unused-label $^ -o $@

$(C_EXAMPLE): examples/c_regions.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c examples/c_regions.c -o $(BUILD_DIR)/c_regions.o
	$(CXX) $(BUILD_DIR)/c_regions.o $(STATIC_LIB) $(LDLIBS) -o $@

$(CPP_EXAMPLE): examples/cpp_pmr.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(TRACE_EXAMPLE): examples/traced_matmul.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BLOCKED_MATMUL): examples/traced_blocked_matmul.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BLOCKED_MATMUL_TRACE): $(BLOCKED_MATMUL)
	$(BLOCKED_MATMUL) --trace $@

$(TILED_MATMUL): examples/traced_tiled_matmul.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(TILED_MATMUL_TRACE): $(TILED_MATMUL)
	$(TILED_MATMUL) --trace $@

$(STRUCTURE_BENCH): examples/traced_structures.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(STRUCTURE_VARIABLE_BENCH): examples/traced_structures_variable.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BFS_TRACE): $(STRUCTURE_VARIABLE_BENCH)
	$(STRUCTURE_VARIABLE_BENCH) bfs $@

$(HASH_JOIN_TRACE): $(STRUCTURE_VARIABLE_BENCH)
	$(STRUCTURE_VARIABLE_BENCH) hash_join $@

$(STENCIL_TRACE): $(STRUCTURE_VARIABLE_BENCH)
	$(STRUCTURE_VARIABLE_BENCH) stencil $@

$(XRT_EXAMPLE): examples/xrt_vertical_slice.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(XRT_MIGRATION_BENCH): examples/xrt_migration_benchmark.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(XRT_BLOCKED_MATMUL): examples/xrt_blocked_matmul.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

test: $(SIM) $(C_API_TEST) $(CPP_API_TEST) $(CONCURRENCY_TEST) $(TRACE_API_TEST) $(HLS_MATMUL_TEST) $(BENCHMARK_TRACES) $(BLOCKED_MATMUL_TRACE) $(TILED_MATMUL_TRACE)
	$(SIM) --self-test
	$(SIM) --trace traces/smoke.trace --profile profiles/default.profile
	$(C_API_TEST)
	$(CPP_API_TEST)
	$(CONCURRENCY_TEST)
	$(TRACE_API_TEST) $(BUILD_DIR)/test_workload.rttrace
	$(HLS_MATMUL_TEST)
	$(SIM) --trace $(BUILD_DIR)/test_workload.rttrace --policy hint
	$(SIM) --trace $(BUILD_DIR)/test_workload.rttrace --policy oracle
	$(SIM) --trace $(BFS_TRACE) --policy hint
	$(SIM) --trace $(HASH_JOIN_TRACE) --policy hint
	$(SIM) --trace $(STENCIL_TRACE) --policy hint
	$(SIM) --trace $(STENCIL_TRACE) --policy oracle
	sh tests/test_benchmark_replay.sh $(STRUCTURE_VARIABLE_BENCH) $(SIM) $(BUILD_DIR)
	sh tests/test_blocked_matmul.sh $(BLOCKED_MATMUL) $(SIM) $(BUILD_DIR) profiles/large_experiment.profile
	sh tests/test_tiled_matmul.sh $(TILED_MATMUL) $(SIM) $(BUILD_DIR) profiles/large_experiment.profile
	sh tests/test_evidence_replay.sh $(SIM) $(BUILD_DIR) tests/profiles tests/traces
	sh tests/test_lifetime_analysis.sh $(SIM) $(BLOCKED_MATMUL) $(BUILD_DIR) profiles/large_experiment.profile

examples: $(C_EXAMPLE) $(CPP_EXAMPLE) $(TRACE_EXAMPLE) $(BLOCKED_MATMUL) $(TILED_MATMUL) $(STRUCTURE_BENCH) $(STRUCTURE_VARIABLE_BENCH)
	$(C_EXAMPLE)
	$(CPP_EXAMPLE)
	$(TRACE_EXAMPLE)
	$(BLOCKED_MATMUL)
	$(TILED_MATMUL)
	$(STRUCTURE_BENCH) bfs
	$(STRUCTURE_VARIABLE_BENCH) bfs
	$(STRUCTURE_VARIABLE_BENCH) hash_join
	$(STRUCTURE_VARIABLE_BENCH) stencil

trace-example: $(SIM) $(TRACE_EXAMPLE)
	$(TRACE_EXAMPLE) $(TRACE_EXAMPLE_OUTPUT)
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy hint
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy oracle
	$(SIM) --trace $(TRACE_EXAMPLE_OUTPUT) --policy durable

blocked-matmul: $(SIM) $(BLOCKED_MATMUL_TRACE)
	$(SIM) --trace $(BLOCKED_MATMUL_TRACE) --policy hint --profile profiles/large_experiment.profile
	$(SIM) --trace $(BLOCKED_MATMUL_TRACE) --policy durable --profile profiles/large_experiment.profile
	$(SIM) --trace $(BLOCKED_MATMUL_TRACE) --policy oracle --profile profiles/large_experiment.profile

tiled-matmul: $(SIM) $(TILED_MATMUL_TRACE)
	$(SIM) --trace $(TILED_MATMUL_TRACE) --policy hint --profile profiles/large_experiment.profile
	$(SIM) --trace $(TILED_MATMUL_TRACE) --policy durable --profile profiles/large_experiment.profile
	$(SIM) --trace $(TILED_MATMUL_TRACE) --policy oracle --profile profiles/large_experiment.profile

tiled-experiments: $(SIM) $(TILED_MATMUL)
	python3 scripts/run_tiled_matmul_experiments.py \
		--benchmark $(TILED_MATMUL) \
		--sim $(SIM) \
		--sweep-script scripts/run_retention_sweep.py \
		--profile profiles/proposal_equal_resources.profile \
		--output-dir $(BUILD_DIR)/tiled_experiments \
		--dimension 32 --tiles 4,8,16 --panel-width 4

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
xrt-experiments: $(XRT_EXAMPLE) $(XRT_MIGRATION_BENCH) $(XRT_BLOCKED_MATMUL)
else
xrt-example:
	@echo "Rebuild with RTMEM_ENABLE_XRT=1 to enable the XRT example"
	@exit 1
xrt-experiments:
	@echo "Rebuild with RTMEM_ENABLE_XRT=1 to enable the XRT experiments"
	@exit 1
endif

clean:
	rm -rf $(BUILD_DIR)
