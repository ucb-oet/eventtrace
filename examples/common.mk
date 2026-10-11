# Shared build for EventTrace examples: elaborate, generate the C++ decoder, build with Verilator, run, and check.
# An example's Makefile sets SBT_PROJECT, MAIN, and DUT, then includes this file.
# Run after sourcing Chipyard's env.sh, which provides firtool and verilator.

EVENTTRACE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
CHIPYARD_DIR   ?= $(abspath $(EVENTTRACE_DIR)/../..)
BUILD_DIR      := $(abspath build)
RTL_DIR        := $(BUILD_DIR)/rtl
RTL            := $(RTL_DIR)/$(DUT).sv
SIM            := $(BUILD_DIR)/obj_dir/VTestTop
DPI_ADAPTER    := $(EVENTTRACE_DIR)/adapters/verilator/eventtrace_dpi.cc
RUNTIME_DIR    := $(EVENTTRACE_DIR)/runtime
RUNTIME_SRCS   := $(RUNTIME_DIR)/eventtrace/runtime.cc

# Reuse Chipyard's sbt launcher and caches.
SBT ?= java -jar $(CHIPYARD_DIR)/scripts/sbt-launch.jar \
	-Dsbt.ivy.home=$(CHIPYARD_DIR)/.ivy2 \
	-Dsbt.global.base=$(CHIPYARD_DIR)/.sbt \
	-Dsbt.boot.directory=$(CHIPYARD_DIR)/.sbt/boot/ \
	-Dsbt.supershell=false

# Chisel's ChiselStage runs the firtool found here.
export CHISEL_FIRTOOL_PATH ?= $(patsubst %/,%,$(dir $(shell command -v firtool)))

SCALA_SOURCES := $(shell find $(EVENTTRACE_DIR)/frontend src -name '*.scala' -o -name '*.sv')

.PHONY: check run clean
# Each check reads the manifest first, then the simulation log.
check: $(BUILD_DIR)/run.log
	awk -f check.awk $(RTL_DIR)/eventtrace_manifest.json $<

run: $(BUILD_DIR)/run.log

$(BUILD_DIR)/run.log: $(SIM)
	$(SIM) | tee $@

# Start from an empty directory, since the simulator build compiles every .sv file in it.
$(RTL): $(SCALA_SOURCES) $(EVENTTRACE_DIR)/build.sbt
	rm -rf $(RTL_DIR)
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain $(MAIN) $(RTL_DIR)/"

# Verilator 5.022 intermittently aborts with "attempted to destroy locked Thread Pool" when
# verilation itself runs multithreaded (-j 0), so only the C++ build is parallel.
$(SIM): $(RTL) TestTop.sv $(DPI_ADAPTER) $(RUNTIME_SRCS) $(RUNTIME_DIR)/eventtrace/runtime.h
	verilator --main --timing --cc --exe --build --build-jobs 0 --verilate-jobs 1 -Wno-fatal \
		--top-module TestTop -Mdir $(BUILD_DIR)/obj_dir -CFLAGS "-I$(RTL_DIR) -I$(RUNTIME_DIR)" \
		TestTop.sv $(RTL_DIR)/*.sv $(DPI_ADAPTER) $(RUNTIME_SRCS)

clean:
	rm -rf $(BUILD_DIR)
