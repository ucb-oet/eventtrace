# Stage 0/0b: a counter with one two-field event.
SBT_PROJECT := stage0
MAIN        := eventtrace.examples.stage0.Main
DUT         := Counter

include ../common.mk

# The SystemVerilog version: Counter.sv and its C++, checked by the same check.awk.
SV_SOURCES := sv/Counter.sv
SV_CC      := sv/counter.cc
include ../sv.mk
