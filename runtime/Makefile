# Builds and runs the runtime's synthetic tests, which need no simulator. The runtime's sources are in src/; the
# frontend's resources link to them, so that elaboration writes them next to the RTL. The runtime is C++17, as the
# Chisel flow compiles it.
BUILD_DIR := build
CSRC      := src
CXXFLAGS  := -O2 -Wall -Wextra -Werror -I$(CSRC)

.PHONY: test clean
test: $(BUILD_DIR)/runtime_test
	$(BUILD_DIR)/runtime_test

$(BUILD_DIR)/runtime_test: test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc $(CSRC)/eventtrace_runtime.h
	mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 $(CXXFLAGS) -o $@ test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc

clean:
	rm -rf $(BUILD_DIR)
