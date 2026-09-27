module tb;
  localparam integer CASE_MODEL = `CASE_MODEL;
  localparam integer STATE_CASE = `STATE_CASE;
  localparam integer FULL_RESULT = `FULL_RESULT;
  reg clk = 0, rst = 0, en = 0, sel = 0;
  reg [63:0] a = 0, b = 0;
  wire [7:0] y;
  wire [63:0] wide;
  reg [63:0] q = 0, expected;
  integer step;
  top dut(.*);
  task check;
    begin
      expected = a + b;
      if (CASE_MODEL == 1) expected = sel ? expected : a;
      if (CASE_MODEL == 2) expected = expected << 3;
      if (CASE_MODEL == 3) expected = sel ? expected + a : expected * b;
      if (STATE_CASE) expected = q;
      if (y !== expected[7:0] || wide !== (FULL_RESULT ? expected : 64'b0))
        $fatal(1, "scalar fixedpoint Verilog mismatch at %0d", step);
    end
  endtask
  initial begin
    for (step = 0; step < 4096; step = step + 1) begin
      clk = 0;
      a = step * 64'h9e3779b97f4a7c15 ^ 64'h8000000000000000;
      b = step * 64'hd6e8feb86659fd93 + 64'hffffffffffffffff;
      if ((step & 15) == 0) begin a = 0; b = 0; end
      if ((step & 15) == 1) begin a = '1; b = '1; end
      rst = step % 31 == 0;
      en = step % 4 != 0;
      sel = step % 3 != 0;
      #1;
      if (!STATE_CASE || step > 0) check();
      clk = 1;
      q = rst ? b : en ? a + b : q;
      #1;
      check();
      if (STATE_CASE) begin
        a = ~a;
        rst = !rst;
        #1;
        check();
      end
    end
    $display("scalar fixedpoint Verilog oracle passed (4096 cycles)");
    $finish;
  end
endmodule
