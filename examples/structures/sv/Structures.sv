// Structures in SystemVerilog: Structures.scala's pipeline, with each `when` block of its trace a DPI call in the
// branch that selects it. The queries and effects on the fetch queue, the ROB, and the reservation station are in
// the C++.
`include "oet.svh"

module Structures (
  input  logic       clock,
  input  logic       reset,
  output logic [7:0] io_pc
);
`ifdef OET_ENABLE
  import "DPI-C" function void oet_fetch(input longint cycle, input byte unsigned pc);
  import "DPI-C" function void oet_dispatch(input longint cycle, input byte unsigned rob_tail, input byte unsigned pc,
                                            input byte unsigned slot);
  import "DPI-C" function void oet_execute(input longint cycle, input byte unsigned slot, input byte unsigned tag,
                                           input bit mispredict);
  import "DPI-C" function void oet_mispredict(input longint cycle, input byte unsigned cut, input byte unsigned exe_pc);
  import "DPI-C" function void oet_commit(input longint cycle);
`endif
  `OET_SETUP(clock)

  logic [7:0] nextPc;
  logic [7:0] fqPc [4];
  logic [1:0] fqHead;
  logic [2:0] fqCount;

  logic       robValid [8];
  logic       robDone [8];
  logic [7:0] robPc [8];
  logic [3:0] robHead, robTail, robCount;

  logic       rsValid [4];
  logic [7:0] rsPc [4];
  logic [3:0] rsTag [4];
  logic [2:0] rsWait [4];

  assign io_pc    = nextPc;
  assign robCount = robTail - robHead;

  // Execute: the lowest slot whose wait has run out.
  logic       exe, mispredict;
  logic [1:0] exeSlot;
  logic [7:0] exePc;
  logic [3:0] exeTag, cut;
  always_comb begin
    exe     = 1'b0;
    exeSlot = 2'd0;
    for (int s = 3; s >= 0; s--)
      if (rsValid[s] && rsWait[s] == 3'd0) begin
        exe     = 1'b1;
        exeSlot = 2'(s);
      end
  end
  assign exePc      = rsPc[exeSlot];
  assign exeTag     = rsTag[exeSlot];
  assign mispredict = exe && exePc[2:0] == 3'd5;
  assign cut        = exeTag + 4'd1;

  // Commit: the ROB's head, once done.
  logic [2:0] headIdx;
  logic       commit;
  assign headIdx = robHead[2:0];
  assign commit  = robValid[headIdx] && robDone[headIdx];

  // Dispatch: the fetch queue's head into the ROB's tail and the lowest free slot.
  logic       anyFree, dispatch, fetch;
  logic [1:0] dispSlot;
  logic [7:0] dispPc;
  always_comb begin
    anyFree  = 1'b0;
    dispSlot = 2'd0;
    for (int s = 3; s >= 0; s--)
      if (!rsValid[s]) begin
        anyFree  = 1'b1;
        dispSlot = 2'(s);
      end
  end
  assign dispPc   = fqPc[fqHead];
  assign dispatch = fqCount != 3'd0 && robCount != 4'd8 && anyFree && !mispredict;

  // Fetch: the next PC into the fetch queue's tail.
  assign fetch = fqCount != 3'd4 && !mispredict;

  always_ff @(posedge clock) begin
    if (reset) begin
      nextPc  <= 8'd0;
      fqHead  <= 2'd0;
      fqCount <= 3'd0;
      robHead <= 4'd0;
      robTail <= 4'd0;
      for (int e = 0; e < 8; e++) robValid[e] <= 1'b0;
      for (int s = 0; s < 4; s++) rsValid[s] <= 1'b0;
    end
    else begin
      for (int s = 0; s < 4; s++) if (rsValid[s] && rsWait[s] != 3'd0) rsWait[s] <= rsWait[s] - 3'd1;
      if (exe) begin
        rsValid[exeSlot]     <= 1'b0;
        robDone[exeTag[2:0]] <= 1'b1;
      end
      if (commit) begin
        robValid[headIdx] <= 1'b0;
        robHead           <= robHead + 4'd1;
      end
      if (mispredict) begin
        for (int e = 0; e < 8; e++) if ({1'b0, 3'(3'(e) - cut[2:0])} < 4'(robTail - cut)) robValid[e] <= 1'b0;
        robTail <= cut;
        for (int s = 0; s < 4; s++) if (rsPc[s] > exePc) rsValid[s] <= 1'b0;
        fqCount <= 3'd0;
        nextPc  <= exePc + 8'd1;
      end
      else begin
        if (dispatch) begin
          robValid[robTail[2:0]] <= 1'b1;
          robDone[robTail[2:0]]  <= 1'b0;
          robPc[robTail[2:0]]    <= dispPc;
          robTail                <= robTail + 4'd1;
          rsValid[dispSlot]      <= 1'b1;
          rsPc[dispSlot]         <= dispPc;
          rsTag[dispSlot]        <= robTail;
          rsWait[dispSlot]       <= (dispPc[2:0] ^ 3'd5) | {dispPc[3], 2'b00};
          fqHead                 <= fqHead + 2'd1;
        end
        if (fetch) begin
          fqPc[2'(fqHead + fqCount[1:0])] <= nextPc;
          nextPc                          <= nextPc + 8'd1;
        end
        fqCount <= fqCount + 3'(fetch) - 3'(dispatch);
      end

      if (fetch)      `OET(oet_fetch(oet_cycle, nextPc))
      if (dispatch)   `OET(oet_dispatch(oet_cycle, 8'(robTail), dispPc, 8'(dispSlot)))
      if (exe)        `OET(oet_execute(oet_cycle, 8'(exeSlot), 8'(exeTag), mispredict))
      if (mispredict) `OET(oet_mispredict(oet_cycle, 8'(cut), exePc))
      if (commit)     `OET(oet_commit(oet_cycle))
    end
  end
endmodule
