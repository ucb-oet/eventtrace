# Stage 1: event sites under when/elsewhen/otherwise.
SBT_PROJECT := stage1
MAIN        := eventtrace.examples.stage1.Main
DUT         := Conditional

include ../common.mk

# Designs breaking a frontend rule must fail to elaborate, each with its own error.
.PHONY: check-rejects
check: check-rejects
check-rejects:
	cd $(EVENTTRACE_DIR) && $(SBT) "$(SBT_PROJECT)/runMain eventtrace.examples.stage1.Rejects"
