# Structures: a pipeline tracked through a queue, a ROB, and a map, in Chisel and in SystemVerilog.
SBT_PROJECT := structures
MAIN        := eventtrace.examples.structures.Main
DUT         := Structures

include ../common.mk

# Designs breaking a rule of the new kinds must fail to elaborate, each with its own error; and sequences of records
# that break one at runtime must stop the runtime, each with its own violation.
.PHONY: check-rejects check-violations
check: check-rejects check-violations
check-rejects:
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.structures.Rejects"

MISUSE_DIR := $(BUILD_DIR)/misuse
VIOLATIONS := $(BUILD_DIR)/violations

$(MISUSE_DIR)/eventtrace_sites.h: $(SCALA_SOURCES) $(EVENTTRACE_DIR)/build.sbt
	rm -rf $(MISUSE_DIR)
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.structures.MisuseMain $(MISUSE_DIR)/"

# Elaboration writes the runtime next to the generated header, as for a simulation; only the DPI adapter is left out.
$(VIOLATIONS): violations.cc $(MISUSE_DIR)/eventtrace_sites.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I$(MISUSE_DIR) -o $@ violations.cc $(MISUSE_DIR)/eventtrace_runtime.cc

check-violations: $(VIOLATIONS) $(EVENTTRACE)
	$(VIOLATIONS) "$(DUMP)" $(MISUSE_DIR)/eventtrace_manifest.json $(BUILD_DIR)

# The SystemVerilog version: Structures.sv and its C++, checked by the same check.awk.
SV_SOURCES := sv/Structures.sv
SV_CC      := sv/structures.cc
include ../sv.mk

# The same sequences against the C++ library, which the SystemVerilog version uses.
SV_VIOLATIONS := $(BUILD_DIR)/sv_violations
$(SV_VIOLATIONS): sv/violations.cc $(OET_CC) $(OET_HEADERS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(OET_CFLAGS) -O2 -Wall -Wextra -Werror -o $@ sv/violations.cc $(OET_CC)

.PHONY: check-sv-violations
check-sv: check-sv-violations
check-sv-violations: $(SV_VIOLATIONS) $(EVENTTRACE)
	$(SV_VIOLATIONS) "$(DUMP)" $(BUILD_DIR)
