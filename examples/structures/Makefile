# Structures: a pipeline tracked through a queue, a ROB, and a map.
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
