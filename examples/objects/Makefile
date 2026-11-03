# First objects: create events for two object types.
SBT_PROJECT := objects
MAIN        := eventtrace.examples.objects.Main
DUT         := Arrivals

include ../common.mk

# Designs breaking an object rule must fail to elaborate, each with its own error.
.PHONY: check-rejects
check: check-rejects
check-rejects:
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.objects.Rejects"

# The SystemVerilog version: Arrivals.sv and its C++, checked by the same check.awk.
SV_SOURCES := sv/Arrivals.sv
SV_CC      := sv/arrivals.cc
include ../sv.mk
