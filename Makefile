CXX ?= g++
CC ?= cc
AR ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude
LDLIBS ?=
RTMEM_ENABLE_XRT ?= 0
XRT_ROOT ?= /opt/xilinx/xrt
XRT_LIB_DIR ?= $(XRT_ROOT)/lib

BUILD_DIR := build
SIM := $(BUILD_DIR)/retention_sim
SIM_SOURCES := sim/main.cpp sim/retention_model.cpp
LIB_OBJECTS := $(BUILD_DIR)/rtmem.o $(BUILD_DIR)/rtmem_cpp.o \
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
C_EXAMPLE := $(BUILD_DIR)/c_regions
CPP_EXAMPLE := $(BUILD_DIR)/cpp_pmr
XRT_EXAMPLE := $(BUILD_DIR)/xrt_vertical_slice

.PHONY: all test examples xrt-example clean

all: $(SIM) $(STATIC_LIB) $(SHARED_LIB)

$(SIM): $(SIM_SOURCES) sim/retention_model.hpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isim $(SIM_SOURCES) -o $(SIM)

$(BUILD_DIR)/rtmem.o: src/rtmem.cpp src/backend.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/rtmem_cpp.o: src/rtmem_cpp.cpp include/rtmem/rtmem.hpp include/rtmem/rtmem.h
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

$(C_EXAMPLE): examples/c_regions.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c examples/c_regions.c -o $(BUILD_DIR)/c_regions.o
	$(CXX) $(BUILD_DIR)/c_regions.o $(STATIC_LIB) $(LDLIBS) -o $@

$(CPP_EXAMPLE): examples/cpp_pmr.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(XRT_EXAMPLE): examples/xrt_vertical_slice.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

test: $(SIM) $(C_API_TEST) $(CPP_API_TEST) $(CONCURRENCY_TEST)
	$(SIM) --self-test
	$(SIM) --trace traces/smoke.trace
	$(C_API_TEST)
	$(CPP_API_TEST)
	$(CONCURRENCY_TEST)

examples: $(C_EXAMPLE) $(CPP_EXAMPLE)
	$(C_EXAMPLE)
	$(CPP_EXAMPLE)

ifeq ($(RTMEM_ENABLE_XRT),1)
xrt-example: $(XRT_EXAMPLE)
else
xrt-example:
	@echo "Rebuild with RTMEM_ENABLE_XRT=1 to enable the XRT example"
	@exit 1
endif

clean:
	rm -rf $(BUILD_DIR)
