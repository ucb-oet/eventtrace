// Collections in SystemVerilog: Pipeline.scala's pipeline, with each of its sites a DPI call in the branch that
// selects it. The queries, effects, and tests are in the C++.
`include "oet.svh"

module Pipeline (
  input  logic       clock,
  input  logic       reset,
  output logic [7:0] io_count
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_decode(input longint cycle, input byte unsigned pc);
  import "DPI-C" function void oet_issue(input longint cycle);
  import "DPI-C" function void oet_writeback(input longint cycle, input byte unsigned pc, input byte unsigned result);
  import "DPI-C" function void oet_retire(input longint cycle);
  import "DPI-C" function void oet_squash(input longint cycle);
  import "DPI-C" function void oet_stall(input longint cycle);
  import "DPI-C" function void oet_sample(input longint cycle);
  import "DPI-C" function void oet_second(input longint cycle);
  import "DPI-C" function void oet_every3(input longint cycle, input byte unsigned exe_pc, input byte unsigned count);
  import "DPI-C" function void oet_request_issue(input longint cycle);
  import "DPI-C" function void oet_request_done(input longint cycle);
  import "DPI-C" function void oet_pick(input longint cycle, input bit oldest);
`endif
  `OET_SETUP(clock)

  logic [7:0] count;
  logic       decV, exeV, wbV;
  logic [7:0] decPc, exePc;  // not reset, as Pipeline.scala's Regs
  logic       stalled;       // stall at the previous edge, as Utils.rising keeps it
  logic       pending;       // a request is outstanding

  logic kill, stall, issue, enter, issueRequest, requestDone;
  assign kill         = exeV && count % 8'd11 == 8'd5;
  assign stall        = decV && exeV && !kill && count % 8'd7 == 8'd3;
  assign issue        = decV && !stall && !kill;
  assign enter        = !stall && !kill;
  assign issueRequest = exeV && count % 8'd4 == 8'd1;
  assign requestDone  = pending && count % 8'd4 == 8'd3;
  assign io_count     = count;

  always_ff @(posedge clock) begin
    exePc <= decPc;
    if (enter) decPc <= count;
  end

  always_ff @(posedge clock) begin
    if (reset) begin
      count   <= 8'd0;
      decV    <= 1'b0;
      exeV    <= 1'b0;
      wbV     <= 1'b0;
      stalled <= 1'b0;
      pending <= 1'b0;
    end
    else begin
      count   <= count + 8'd1;
      wbV     <= exeV;
      exeV    <= issue;
      decV    <= !kill;
      stalled <= stall;
      if (issueRequest) pending <= 1'b1;
      if (requestDone)  pending <= 1'b0;

      if (enter)            `OET(oet_decode(oet_cycle, count))
      if (issue)            `OET(oet_issue(oet_cycle))
      if (exeV)             `OET(oet_writeback(oet_cycle, exePc, exePc + 8'd1))
      if (wbV)              `OET(oet_retire(oet_cycle))
      if (kill)             `OET(oet_squash(oet_cycle))
      if (stall && !stalled) `OET(oet_stall(oet_cycle))
      if (count % 8'd5 == 8'd0) `OET(oet_sample(oet_cycle))
      if (count % 8'd4 == 8'd2 && 2'(decV) + 2'(exeV) + 2'(wbV) >= 2'd2) `OET(oet_second(oet_cycle))
      if (count % 8'd3 == 8'd0) `OET(oet_every3(oet_cycle, exePc, count))
      if (issueRequest)     `OET(oet_request_issue(oet_cycle))
      if (requestDone)      `OET(oet_request_done(oet_cycle))
      if (count % 8'd5 == 8'd1 && decV) `OET(oet_pick(oet_cycle, count[1]))
    end
  end
endmodule
