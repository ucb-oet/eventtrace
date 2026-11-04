// Stage 1 in SystemVerilog: Conditional.scala's sites, each a DPI call in the branch of the always_ff that selects it.
`include "oet.svh"

// Instantiated twice; its parent gives each instance its index, which picks its own site.
module Lane #(
  parameter int Index = 0
) (
  input logic       clock,
  input logic       reset,
  input logic [7:0] in
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_lane(input longint cycle, input int index, input byte unsigned value);
  `OET_SETUP(clock)
  always @(posedge clock)
    if (!reset && in[0]) oet_lane(oet_cycle, Index, in);
`endif
endmodule

module Conditional (
  input  logic       clock,
  input  logic       reset,
  output logic [7:0] io_count
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_tick(input longint cycle);
  import "DPI-C" function void oet_first(input longint cycle, input byte unsigned count);
  import "DPI-C" function void oet_second(input longint cycle, input byte unsigned count, input byte unsigned sum);
  import "DPI-C" function void oet_rest_even(input longint cycle, input byte unsigned phase, input bit odd);
  import "DPI-C" function void oet_rest_odd(input longint cycle, input byte unsigned phase, input bit odd);
  import "DPI-C" function void oet_mode(input longint cycle, input byte unsigned mode);
  import "DPI-C" function void oet_wide(input longint cycle, input int unsigned a, input longint unsigned b,
                                        input byte unsigned c);
  import "DPI-C" function void oet_flush_absent(input longint cycle);
  import "DPI-C" function void oet_flush(input longint cycle, input byte unsigned cause);
  import "DPI-C" function void oet_rise1(input longint cycle);
  import "DPI-C" function void oet_rise2(input longint cycle);
  import "DPI-C" function void oet_fall2(input longint cycle);
`endif
  `OET_SETUP(clock)

  logic [7:0] count;
  logic [1:0] phase;
  logic       was1, was2;  // count bits 1 and 2 at the previous edge, as Utils.rising and falling keep them

  assign phase    = count[1:0];
  assign io_count = count;

  always_ff @(posedge clock) begin
    if (reset) begin
      count <= 8'd0;
      was1  <= 1'b0;
      was2  <= 1'b0;
    end
    else begin
      count <= count + 8'd1;
      was1  <= count[1];
      was2  <= count[2];
      `OET(oet_tick(oet_cycle))
      if (count[2]) begin
        if (phase == 2'd0)      `OET(oet_first(oet_cycle, count))
        else if (phase == 2'd1) `OET(oet_second(oet_cycle, count, count + 8'd3))
        else if (phase == 2'd2) `OET(oet_rest_even(oet_cycle, 8'(phase), count[0]))
        else                    `OET(oet_rest_odd(oet_cycle, 8'(phase), count[0]))
      end
      // count mod 4 as a Mode, of which 3 is no variant.
      if (count[3]) `OET(oet_mode(oet_cycle, 8'(count[1:0])))
      if (count[4]) `OET(oet_wide(oet_cycle, {count, count, count, count},
                                  64'({count[3:0], count, count, count, count}), count))
      if (count == 8'd5) `OET(oet_flush_absent(oet_cycle))
      if (count == 8'd6) `OET(oet_flush(oet_cycle, 8'(count[1:0])))
      if (count[1] && !was1) `OET(oet_rise1(oet_cycle))
      if (count[2] && !was2) `OET(oet_rise2(oet_cycle))
      if (!count[2] && was2) `OET(oet_fall2(oet_cycle))
    end
  end

  // lane0 sees odd values on odd cycles and lane1 on even cycles, so exactly one lane fires each cycle.
  Lane #(.Index(0)) lane0 (.clock, .reset, .in(count));
  Lane #(.Index(1)) lane1 (.clock, .reset, .in(count + 8'd1));
endmodule
