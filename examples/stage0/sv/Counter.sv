// Stage 0 in SystemVerilog: the counter of Counter.scala, with its tick traced through a DPI call.
`include "oet.svh"

module Counter (
  input  logic        clock,
  input  logic        reset,
  output logic [31:0] io_count
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_tick(input longint cycle, input int unsigned count, input byte delta);
`endif
  `OET_CLOCK(clock)

  // Starts near the top of the range, as the Chisel counter does.
  logic [31:0] count;

  // Every eighth cycle out of reset, a tick: `delta` is the low byte as a signed value minus 20, wrapping as an 8-bit
  // value. The call sits in the branch where the tick happens, so the branch's conditions are its condition.
  always_ff @(posedge clock) begin
    if (reset) begin
      count <= 32'hffffffe0;
    end
    else begin
      count <= count + 32'd1;
      if (count[2:0] == 3'd0) `OET(oet_tick(oet_cycle, count, byte'(count[7:0]) - 8'sd20))
    end
  end
  assign io_count = count;
endmodule
