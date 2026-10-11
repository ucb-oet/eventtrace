# Builds and runs the runtime's synthetic tests, which need no simulator. The runtime ships in the frontend's
# resources, so that elaboration writes it next to the RTL.
BUILD_DIR := build
CSRC      := ../frontend/src/main/resources/eventtrace/csrc
CXXFLAGS  := -std=c++17 -O2 -Wall -Wextra -Werror -I$(CSRC)

.PHONY: test clean
test: $(BUILD_DIR)/runtime_test
	$<

$(BUILD_DIR)/runtime_test: test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc $(CSRC)/eventtrace_runtime.h
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc

clean:
	rm -rf $(BUILD_DIR)
