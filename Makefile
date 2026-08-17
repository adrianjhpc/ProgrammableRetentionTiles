CXX ?= g++
CC ?= cc
AR ?= ar
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude

BUILD_DIR := build
SIM := $(BUILD_DIR)/retention_sim
SIM_SOURCES := sim/main.cpp sim/retention_model.cpp
LIB_OBJECTS := $(BUILD_DIR)/rtmem.o $(BUILD_DIR)/rtmem_cpp.o
STATIC_LIB := $(BUILD_DIR)/librtmem.a
SHARED_LIB := $(BUILD_DIR)/librtmem.so
C_API_TEST := $(BUILD_DIR)/test_c_api
CPP_API_TEST := $(BUILD_DIR)/test_cpp_api
CONCURRENCY_TEST := $(BUILD_DIR)/test_concurrency
C_EXAMPLE := $(BUILD_DIR)/c_regions
CPP_EXAMPLE := $(BUILD_DIR)/cpp_pmr

.PHONY: all test examples clean

all: $(SIM) $(STATIC_LIB) $(SHARED_LIB)

$(SIM): $(SIM_SOURCES) sim/retention_model.hpp
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isim $(SIM_SOURCES) -o $(SIM)

$(BUILD_DIR)/rtmem.o: src/rtmem.cpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/rtmem_cpp.o: src/rtmem_cpp.cpp include/rtmem/rtmem.hpp include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJECTS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJECTS)
	$(CXX) -shared $^ -o $@

$(BUILD_DIR)/test_c_api.o: tests/test_c_api.c include/rtmem/rtmem.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(C_API_TEST): $(BUILD_DIR)/test_c_api.o $(STATIC_LIB)
	$(CXX) $^ -o $@

$(CPP_API_TEST): tests/test_cpp_api.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@

$(CONCURRENCY_TEST): tests/test_concurrency.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -pthread $^ -o $@

$(C_EXAMPLE): examples/c_regions.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c examples/c_regions.c -o $(BUILD_DIR)/c_regions.o
	$(CXX) $(BUILD_DIR)/c_regions.o $(STATIC_LIB) -o $@

$(CPP_EXAMPLE): examples/cpp_pmr.cpp $(STATIC_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $^ -o $@

test: $(SIM) $(C_API_TEST) $(CPP_API_TEST) $(CONCURRENCY_TEST)
	$(SIM) --self-test
	$(SIM) --trace traces/smoke.trace
	$(C_API_TEST)
	$(CPP_API_TEST)
	$(CONCURRENCY_TEST)

examples: $(C_EXAMPLE) $(CPP_EXAMPLE)
	$(C_EXAMPLE)
	$(CPP_EXAMPLE)

clean:
	rm -rf $(BUILD_DIR)
