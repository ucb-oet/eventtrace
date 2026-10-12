# Shared build for EventTrace examples: elaborate, build with Verilator, run, and check.
# An example's Makefile sets SBT_PROJECT, MAIN, and DUT, then includes this file.
# Run after sourcing Chipyard's env.sh, which provides firtool and verilator, with cargo installed for the reader.
#
# Elaboration writes everything the simulator compiles into RTL_DIR, as Chipyard's flow would: the RTL, the sinks'
# SystemVerilog, the runtime and DPI adapter, and the generated eventtrace_sites.h. Each example's main then writes
# the manifest there too.

EVENTTRACE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
CHIPYARD_DIR   ?= $(abspath $(EVENTTRACE_DIR)/../..)
BUILD_DIR      := $(abspath build)
RTL_DIR        := $(BUILD_DIR)/rtl
RTL            := $(RTL_DIR)/$(DUT).sv
SIM            := $(BUILD_DIR)/obj_dir/VTestTop

# Reuse Chipyard's sbt launcher and caches.
SBT ?= java -jar $(CHIPYARD_DIR)/scripts/sbt-launch.jar \
	-Dsbt.ivy.home=$(CHIPYARD_DIR)/.ivy2 \
	-Dsbt.global.base=$(CHIPYARD_DIR)/.sbt \
	-Dsbt.boot.directory=$(CHIPYARD_DIR)/.sbt/boot/ \
	-Dsbt.supershell=false

# Chisel's ChiselStage runs the firtool found here.
export CHISEL_FIRTOOL_PATH ?= $(patsubst %/,%,$(dir $(shell command -v firtool)))

# The frontend's sources include the runtime's C++, which it ships as resources.
SCALA_SOURCES := $(shell find $(EVENTTRACE_DIR)/frontend/src src -type f)

# The reader's command, which Cargo rebuilds when the reader or the trace format changes.
READER_DIR := $(EVENTTRACE_DIR)/reader
EVENTTRACE := $(READER_DIR)/target/release/eventtrace
READER_SOURCES := $(shell find $(READER_DIR)/src $(EVENTTRACE_DIR)/proto -type f) \
	$(addprefix $(READER_DIR)/,build.rs Cargo.toml Cargo.lock)

# Prints a trace as text, given its manifest.
DUMP := $(EVENTTRACE) dump

.PHONY: check run clean
# Each check reads the manifest first, then the trace as text.
check: $(BUILD_DIR)/run.log
	awk -f check.awk $(RTL_DIR)/eventtrace_manifest.json $<

run: $(BUILD_DIR)/run.log

$(BUILD_DIR)/run.trace: $(SIM)
	$(SIM) +eventtrace=$@

$(BUILD_DIR)/run.log: $(BUILD_DIR)/run.trace $(EVENTTRACE)
	$(DUMP) $(RTL_DIR)/eventtrace_manifest.json $< > $@

$(EVENTTRACE): $(READER_SOURCES)
	cargo build --release --quiet --manifest-path $(READER_DIR)/Cargo.toml

# Start from an empty directory, since the simulator build compiles every .sv and .cc file in it.
$(RTL): $(SCALA_SOURCES) $(EVENTTRACE_DIR)/build.sbt
	rm -rf $(RTL_DIR)
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain $(MAIN) $(RTL_DIR)/"

# Verilator 5.022 intermittently aborts with "attempted to destroy locked Thread Pool" when
# verilation itself runs multithreaded (-j 0), so only the C++ build is parallel.
$(SIM): $(RTL) TestTop.sv
	verilator --main --timing --cc --exe --build --build-jobs 0 --verilate-jobs 1 -Wno-fatal \
		--top-module TestTop -Mdir $(BUILD_DIR)/obj_dir -CFLAGS "-I$(RTL_DIR)" \
		TestTop.sv $(RTL_DIR)/*.sv $(RTL_DIR)/*.cc

clean:
	rm -rf $(BUILD_DIR)
