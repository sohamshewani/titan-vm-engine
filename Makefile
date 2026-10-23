CXX := g++
CXXFLAGS := -O3 -Wall -Wextra -std=c++17 -MMD -MP
INCLUDES := -Iinclude
LDFLAGS := 
LIBS := 

SRC_DIR := src
BUILD_DIR := build
TEST_DIR := tests
BENCH_DIR := benchmarks
BIN_DIR := bin

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

TEST_SRCS := $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS := $(TEST_SRCS:$(TEST_DIR)/%.cpp=$(BUILD_DIR)/tests/%.o)
TEST_DEPS := $(TEST_OBJS:.o=.d)

BENCH_SRCS := $(wildcard $(BENCH_DIR)/*.cpp)
BENCH_OBJS := $(BENCH_SRCS:$(BENCH_DIR)/%.cpp=$(BUILD_DIR)/benchmarks/%.o)
BENCH_DEPS := $(BENCH_OBJS:.o=.d)

TARGET := $(BIN_DIR)/titan-vm-engine
TEST_TARGET := $(BIN_DIR)/run_tests
BENCH_TARGET := $(BIN_DIR)/run_benchmarks

.PHONY: all build test benchmark clean dirs

all: dirs build test benchmark

dirs:
	@mkdir -p $(BUILD_DIR)/tests $(BUILD_DIR)/benchmarks $(BIN_DIR)

build: dirs $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

test: dirs $(TEST_TARGET)
	@if [ -x "$(TEST_TARGET)" ]; then \
		./$(TEST_TARGET); \
	else \
		echo "No tests found or test target failed to build."; \
	fi

$(TEST_TARGET): $(OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) $(TEST_OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

benchmark: dirs $(BENCH_TARGET)
	@if [ -x "$(BENCH_TARGET)" ]; then \
		./$(BENCH_TARGET); \
	else \
		echo "No benchmarks found or benchmark target failed to build."; \
	fi

$(BENCH_TARGET): $(OBJS) $(BENCH_OBJS)
	$(CXX) $(CXXFLAGS) $(filter-out $(BUILD_DIR)/main.o, $(OBJS)) $(BENCH_OBJS) -o $@ $(LDFLAGS) $(LIBS)

$(BUILD_DIR)/benchmarks/%.o: $(BENCH_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS) $(TEST_DEPS) $(BENCH_DEPS)