# Shared build for the examples' SystemVerilog versions: the design in sv/, its C++ in sv/, and the same TestTop and
# check as the Chisel version. An example's Makefile sets SV_SOURCES and SV_CC, then includes this file after
# common.mk. check-sv builds with OET_ENABLE and checks the trace; check-sv-off builds without it and checks that
# nothing of the trace is compiled.

SV_DIR       := $(BUILD_DIR)/sv
SV_SIM       := $(SV_DIR)/obj_dir/VTestTop
SV_OFF_DIR   := $(BUILD_DIR)/sv_off
SV_OFF_SIM   := $(SV_OFF_DIR)/obj_dir/VTestTop
OET_INCLUDE  := $(EVENTTRACE_DIR)/systemverilog
RUNTIME_SRC  := $(EVENTTRACE_DIR)/runtime/src
OET_CC       := $(RUNTIME_SRC)/oet.cc $(RUNTIME_SRC)/eventtrace_runtime.cc
OET_HEADERS  := $(wildcard $(RUNTIME_SRC)/*.h) $(OET_INCLUDE)/oet.svh

VERILATE := verilator --main --timing --cc --exe --build --build-jobs 0 --verilate-jobs 1 -Wno-fatal \
	--top-module TestTop -I$(OET_INCLUDE)

.PHONY: check-sv check-sv-off
check-sv: $(SV_DIR)/run.log
	awk -f check.awk $(SV_DIR)/run.trace.json $<

$(SV_SIM): TestTop.sv $(SV_SOURCES) $(SV_CC) $(OET_CC) $(OET_HEADERS)
	mkdir -p $(SV_DIR)
	$(VERILATE) +define+OET_ENABLE -Mdir $(SV_DIR)/obj_dir -CFLAGS "-std=c++20 -I$(RUNTIME_SRC)" \
		TestTop.sv $(SV_SOURCES) $(abspath $(SV_CC)) $(OET_CC)

# The library writes the manifest beside the trace, as run.trace.json.
$(SV_DIR)/run.trace: $(SV_SIM)
	$(SV_SIM) +eventtrace=$@

$(SV_DIR)/run.log: $(SV_DIR)/run.trace $(EVENTTRACE)
	$(DUMP) $(SV_DIR)/run.trace.json $< > $@

# Without OET_ENABLE, the build takes no C++ of the trace, and the verilated model mentions none of its names.
check-sv-off: TestTop.sv $(SV_SOURCES) $(OET_INCLUDE)/oet.svh
	rm -rf $(SV_OFF_DIR)
	mkdir -p $(SV_OFF_DIR)
	$(VERILATE) -Mdir $(SV_OFF_DIR)/obj_dir TestTop.sv $(SV_SOURCES) > $(SV_OFF_DIR).log 2>&1
	$(SV_OFF_SIM) +eventtrace=$(SV_OFF_DIR)/run.trace > /dev/null
	@if grep -rl 'oet_\|eventtrace' $(SV_OFF_DIR)/obj_dir --include='*.h' --include='*.cpp' | grep . ; then \
		echo "FAIL: the build without OET_ENABLE compiled trace code"; exit 1; fi
	@if [ -e $(SV_OFF_DIR)/run.trace ]; then echo "FAIL: the build without OET_ENABLE wrote a trace"; exit 1; fi
	@echo "PASS: without OET_ENABLE, the model has no DPI import or code of the trace, and runs without writing one"
