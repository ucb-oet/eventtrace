# Builds and runs the runtime's synthetic tests, which need no simulator. The runtime's sources are in src/; the
# frontend's resources link to them, so that elaboration writes them next to the RTL. The runtime is C++17, as the
# Chisel flow compiles it; the C++ library, oet, is C++20.
BUILD_DIR := build
CSRC      := src
CXXFLAGS  := -O2 -Wall -Wextra -Werror -I$(CSRC)

# The reader, which the library's test reads its trace back with.
READER_DIR := ../reader
EVENTTRACE := $(READER_DIR)/target/release/eventtrace
READER_SOURCES := $(shell find $(READER_DIR)/src ../proto -type f) \
	$(addprefix $(READER_DIR)/,build.rs Cargo.toml Cargo.lock)

.PHONY: test clean
test: $(BUILD_DIR)/runtime_test $(BUILD_DIR)/oet_test $(EVENTTRACE) oet-rejects
	$(BUILD_DIR)/runtime_test
	$(BUILD_DIR)/oet_test $(EVENTTRACE)

# Each case of oet_rejects.cc must stop the run, when the trace opens, with the message it names.
REJECT_CASES := 1 2 3 4 5 6 7 8 9 10 11 12 13 14
.PHONY: oet-rejects
oet-rejects: $(addprefix $(BUILD_DIR)/oet_reject_,$(REJECT_CASES))
	@for n in $(REJECT_CASES); do \
	  out=$$($(BUILD_DIR)/oet_reject_$$n 2>&1); \
	  want=$$(printf '%s\n' "$$out" | sed -n 's/^expect: //p'); \
	  got=$$(printf '%s\n' "$$out" | sed -n 's/^eventtrace: //p'); \
	  if [ -z "$$got" ] || [ "$$got" != "$$want" ]; then echo "FAIL: case $$n: $$out"; exit 1; fi; \
	done; echo "PASS: $(words $(REJECT_CASES)) declarations breaking a rule each stop the run with its message when the trace opens"

$(BUILD_DIR)/oet_reject_%: test/oet_rejects.cc $(CSRC)/oet.cc $(CSRC)/oet.h $(CSRC)/eventtrace_runtime.cc
	mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++20 $(CXXFLAGS) -DCASE=$* -o $@ test/oet_rejects.cc $(CSRC)/oet.cc $(CSRC)/eventtrace_runtime.cc

$(BUILD_DIR)/runtime_test: test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc $(CSRC)/eventtrace_runtime.h
	mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 $(CXXFLAGS) -o $@ test/runtime_test.cc $(CSRC)/eventtrace_runtime.cc

$(BUILD_DIR)/oet_test: test/oet_test.cc $(CSRC)/oet.cc $(CSRC)/oet.h $(CSRC)/eventtrace_runtime.cc \
		$(CSRC)/eventtrace_runtime.h $(CSRC)/eventtrace_tracker.h
	mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++20 $(CXXFLAGS) -o $@ test/oet_test.cc $(CSRC)/oet.cc $(CSRC)/eventtrace_runtime.cc

$(EVENTTRACE): $(READER_SOURCES)
	cargo build --release --quiet --manifest-path $(READER_DIR)/Cargo.toml

clean:
	rm -rf $(BUILD_DIR)
