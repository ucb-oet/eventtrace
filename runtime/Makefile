# Builds and runs the runtime's synthetic tests, which need no simulator.
BUILD_DIR := build
CXXFLAGS  := -std=c++17 -O2 -Wall -Wextra -Werror -I.

.PHONY: test clean
test: $(BUILD_DIR)/runtime_test
	$<

$(BUILD_DIR)/runtime_test: test/runtime_test.cc eventtrace/runtime.cc eventtrace/runtime.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ test/runtime_test.cc eventtrace/runtime.cc

clean:
	rm -rf $(BUILD_DIR)
