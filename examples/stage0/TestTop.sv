// Stage 0 testbench: drives clock and reset from SystemVerilog, as Chipyard's TestDriver does.
module TestTop;
  logic        clock = 1'b0;
  logic        reset = 1'b1;
  logic [31:0] count;

  always #5 clock = ~clock;

  initial begin
    repeat (3) @(negedge clock);
    reset = 1'b0;
    repeat (80) @(negedge clock);
    $finish;
  end

  Counter dut (
    .clock    (clock),
    .reset    (reset),
    .io_count (count)
  );
endmodule
