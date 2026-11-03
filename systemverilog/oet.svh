// oet: EventTrace for SystemVerilog designs. The design calls a DPI function of its own at each traced point, and
// its C++ handles the call with the oet library (oet.h, beside this file).
//
// Everything here is guarded by OET_ENABLE. Without it, `OET_SETUP(...) expands to nothing and `OET(...) to a null
// statement, so a build compiles nothing of the trace and has no DPI symbols. A design's own DPI imports, and any
// always block that exists only to call, go inside `ifdef OET_ENABLE too.
//
//   `include "oet.svh"
//   `ifdef OET_ENABLE
//     import "DPI-C" function void oet_decode(input longint cycle, input int slot, input int unsigned pc);
//   `endif
//   `OET_SETUP(clk_i)
//   always_ff @(posedge clk_i) begin
//     ...
//     else if (dec_fire) begin
//       ...
//       `OET(oet_decode(oet_cycle, 0, dec_pc))
//     end
//   end
//
// A call placed in the branch where the event happens fires under that branch's conditions and sees the values from
// before the clock edge, as a Chisel sink does. For logic written in always_comb, a call goes in its own block:
//
//   `ifdef OET_ENABLE
//     always @(posedge clk_i) if (!rst_i && vkill_valid) oet_vkill(oet_cycle, vkill.vseq_first);
//   `endif
`ifndef OET_SVH
`define OET_SVH

`ifdef OET_ENABLE

// A call, as a statement.
`define OET(call) call;

// Sets up a module that calls. It declares oet_cycle, the edges of `clk since the simulation started, which every
// call passes first. It also opens the trace given as +eventtrace=<path>, with the recording window's triggers
// +eventtrace_start=<trigger> and +eventtrace_stop=<trigger>, as Chisel's sinks do, and finishes it when the
// simulation ends. Each module that calls uses it once; only the first open counts.
`define OET_SETUP(clk) \
  import "DPI-C" function void oet_open(input string path, input string start, input string stop); \
  import "DPI-C" function void oet_finish(); \
  initial begin \
    string oet_path, oet_start, oet_stop; \
    if ($value$plusargs("eventtrace=%s", oet_path)) begin \
      void'($value$plusargs("eventtrace_start=%s", oet_start)); \
      void'($value$plusargs("eventtrace_stop=%s", oet_stop)); \
      oet_open(oet_path, oet_start, oet_stop); \
    end \
  end \
  longint oet_cycle = 0; \
  always @(posedge clk) oet_cycle <= oet_cycle + 1; \
  final oet_finish();

`else

// A null statement, so that `if (c) `OET(...)` stays a statement.
`define OET(call) ;
`define OET_SETUP(clk)

`endif

`endif
