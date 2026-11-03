// Objects in SystemVerilog: Arrivals.scala's three create sites, each a DPI call in the branch that selects it.
`include "oet.svh"

module Arrivals (
  input  logic       clock,
  input  logic       reset,
  output logic [7:0] io_count
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_arrive4(input longint cycle, input byte unsigned seq, input byte unsigned kind);
  import "DPI-C" function void oet_arrive8(input longint cycle, input byte unsigned seq);
  import "DPI-C" function void oet_grant(input longint cycle, input byte unsigned slot);
`endif
  `OET_SETUP(clock)

  logic [7:0] count;
  assign io_count = count;

  always_ff @(posedge clock) begin
    if (reset) begin
      count <= 8'd0;
    end
    else begin
      count <= count + 8'd1;
      if (count[1:0] == 2'd0)  `OET(oet_arrive4(oet_cycle, count, 8'(count[3:2])))  // a packet every 4th cycle
      if (count[2:0] == 3'd0)  `OET(oet_arrive8(oet_cycle, count))                   // and another, without a kind
      if (count % 8'd3 == 8'd0) `OET(oet_grant(oet_cycle, 8'(count[3:0])))           // a token every 3rd cycle
    end
  end
endmodule
