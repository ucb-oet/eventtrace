// Objects testbench: drives clock and reset from SystemVerilog, as Chipyard's TestDriver does.
module TestTop;
  logic       clock = 1'b0;
  logic       reset = 1'b1;
  logic [7:0] count;

  always #5 clock = ~clock;

  // Reset holds for edges 0 to 2, so records come from edges 3 to 51, with the counter at 0 to 48. On the last
  // edge, count 48, all three create sites fire, so its records appear only if the sinks' final blocks flush them.
  initial begin
    repeat (3) @(negedge clock);
    reset = 1'b0;
    repeat (49) @(negedge clock);
    $finish;
  end

  Arrivals dut (
    .clock    (clock),
    .reset    (reset),
    .io_count (count)
  );
endmodule
