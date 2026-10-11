// Stage 1 testbench: drives clock and reset from SystemVerilog, as Chipyard's TestDriver does.
module TestTop;
  logic       clock = 1'b0;
  logic       reset = 1'b1;
  logic [7:0] count;

  always #5 clock = ~clock;

  // Reset holds for edges 0 to 2, then again for edges 27 and 28. Cycle stamps keep counting through both, and no
  // sink emits while reset is asserted: records come from edges 3 to 26 and 29 to 52.
  initial begin
    repeat (3) @(negedge clock);
    reset = 1'b0;
    repeat (24) @(negedge clock);
    reset = 1'b1;
    repeat (2) @(negedge clock);
    reset = 1'b0;
    repeat (24) @(negedge clock);
    $finish;
  end

  Conditional dut (
    .clock    (clock),
    .reset    (reset),
    .io_count (count)
  );
endmodule
