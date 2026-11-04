# Builds and runs the C++ library's tests, which need no simulator. The library, oet, is C++20; it builds on the
# runtime in ../runtime/src, which it shares with the Chisel flow.
BUILD_DIR   := build
RUNTIME_SRC := ../runtime/src
CXXFLAGS    := -std=c++20 -O2 -Wall -Wextra -Werror -I. -I$(RUNTIME_SRC)
LIB_CC      := oet.cc $(RUNTIME_SRC)/eventtrace_runtime.cc
LIB_DEPS    := $(LIB_CC) oet.h $(RUNTIME_SRC)/eventtrace_runtime.h $(RUNTIME_SRC)/eventtrace_tracker.h

# The reader, which the library's test reads its trace back with.
READER_DIR := ../reader
EVENTTRACE := $(READER_DIR)/target/release/eventtrace
READER_SOURCES := $(shell find $(READER_DIR)/src ../proto -type f) \
	$(addprefix $(READER_DIR)/,build.rs Cargo.toml Cargo.lock)

.PHONY: test clean
test: $(BUILD_DIR)/oet_test $(EVENTTRACE) oet-rejects
	$(BUILD_DIR)/oet_test $(EVENTTRACE)

# Each case of oet_rejects.cc must stop the run, when the trace opens, with the message it names.
REJECT_CASES := 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18
.PHONY: oet-rejects
oet-rejects: $(addprefix $(BUILD_DIR)/oet_reject_,$(REJECT_CASES))
	@for n in $(REJECT_CASES); do \
	  out=$$($(BUILD_DIR)/oet_reject_$$n 2>&1); \
	  want=$$(printf '%s\n' "$$out" | sed -n 's/^expect: //p'); \
	  got=$$(printf '%s\n' "$$out" | sed -n 's/^eventtrace: //p'); \
	  if [ -z "$$got" ] || [ "$$got" != "$$want" ]; then echo "FAIL: case $$n: $$out"; exit 1; fi; \
	done; echo "PASS: $(words $(REJECT_CASES)) declarations breaking a rule each stop the run with its message when the trace opens"

$(BUILD_DIR)/oet_reject_%: test/oet_rejects.cc $(LIB_DEPS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DCASE=$* -o $@ test/oet_rejects.cc $(LIB_CC)

$(BUILD_DIR)/oet_test: test/oet_test.cc $(LIB_DEPS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ test/oet_test.cc $(LIB_CC)

$(EVENTTRACE): $(READER_SOURCES)
	cargo build --release --quiet --manifest-path $(READER_DIR)/Cargo.toml

clean:
	rm -rf $(BUILD_DIR)
