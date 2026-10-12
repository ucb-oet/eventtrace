# Collections: a pipeline tracked through queries on one collection.
SBT_PROJECT := collections
MAIN        := eventtrace.examples.collections.Main
DUT         := Pipeline

include ../common.mk

# Designs breaking a frontend rule must fail to elaborate, each with its own error; and sequences of records that
# break a rule of the tracked model must stop the runtime, each with its own violation.
.PHONY: check-rejects check-violations check-window
check: check-rejects check-violations check-window
check-rejects:
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.collections.Rejects"

MISUSE_DIR := $(BUILD_DIR)/misuse
VIOLATIONS := $(BUILD_DIR)/violations

$(MISUSE_DIR)/eventtrace_sites.h: $(SCALA_SOURCES) $(EVENTTRACE_DIR)/build.sbt
	rm -rf $(MISUSE_DIR)
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.collections.MisuseMain $(MISUSE_DIR)/"

# Elaboration writes the runtime next to the generated header, as for a simulation; only the DPI adapter is left out.
$(VIOLATIONS): violations.cc $(MISUSE_DIR)/eventtrace_sites.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I$(MISUSE_DIR) -o $@ violations.cc $(MISUSE_DIR)/eventtrace_runtime.cc

check-violations: $(VIOLATIONS)
	$(VIOLATIONS) $(EVENTTRACE_DIR)/scripts/eventtrace_trace.py $(MISUSE_DIR)/eventtrace_manifest.json $(BUILD_DIR)

# A recording window writes the full run's events from the cycle where its start trigger fires through the cycle where
# its stop trigger fires, which counts only events from the start. Event IDs count from the window's first event, so
# they are left out of the comparison.
WINDOW_START := event:instruction.writeback,result=7,stage=Writeback
WINDOW_STOP  := event:instruction.squash
$(BUILD_DIR)/window.trace: $(SIM)
	$(SIM) +eventtrace=$@ +eventtrace_start=$(WINDOW_START) +eventtrace_stop=$(WINDOW_STOP)

check-window: $(BUILD_DIR)/run.log $(BUILD_DIR)/window.trace
	$(DUMP) $(RTL_DIR)/eventtrace_manifest.json $(BUILD_DIR)/window.trace | sed 's/ event_id=[0-9]*//' > $(BUILD_DIR)/window.log
	awk 'NR == FNR { c = substr($$2, 7) + 0; if (!s && / instruction.writeback .*result=7 /) s = c; \
	                 if (s && !t && / instruction.squash /) t = c; next } \
	     { c = substr($$2, 7) + 0 } s && t && c >= s && c <= t' $(BUILD_DIR)/run.log $(BUILD_DIR)/run.log \
		| sed 's/ event_id=[0-9]*//' | diff - $(BUILD_DIR)/window.log
	@echo "PASS: the window holds the full run's cycles $$(head -1 $(BUILD_DIR)/window.log | cut -d' ' -f2) to" \
		"$$(tail -1 $(BUILD_DIR)/window.log | cut -d' ' -f2), $$(wc -l < $(BUILD_DIR)/window.log) events"
