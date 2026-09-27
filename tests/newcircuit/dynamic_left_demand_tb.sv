module tb;
  localparam integer CASE_ID = `CASE_ID;
  reg [127:0] a;
  reg [7:0] amount;
  wire [3:0] low, right;
  wire [7:0] middle;
  wire [127:0] wide;
  reg [63:0] lo, hi;
  reg [127:0] expected_left, expected_right, expected_wide;
  reg [7:0] expected_middle;
  reg [7:0] amounts [0:15];
  integer step;
  top dut(.a(a), .amount(amount), .low(low), .middle(middle), .right(right), .wide(wide));
  initial begin
    amounts[0] = 0; amounts[1] = 1; amounts[2] = 3; amounts[3] = 4;
    amounts[4] = 7; amounts[5] = 8; amounts[6] = 15; amounts[7] = 16;
    amounts[8] = 31; amounts[9] = 32; amounts[10] = 63; amounts[11] = 64;
    amounts[12] = 127; amounts[13] = 128; amounts[14] = 129; amounts[15] = 255;
    for (step = 0; step < 4096; step = step + 1) begin
      lo = step * 64'h9e3779b97f4a7c15 ^ 64'hffffffffffffffff;
      hi = step * 64'hd6e8feb86659fd93 ^ 64'h8000000000000000;
      a = {hi, lo};
      amount = amounts[step & 15];
      expected_left = a << amount;
      expected_right = a >> amount;
      expected_wide = CASE_ID == 2 ? expected_left : 128'b0;
      expected_middle = CASE_ID == 0 ? 8'b0 : expected_left[15:8];
      #1;
      if (low !== expected_left[3:0] || right !== expected_right[3:0] ||
          middle !== expected_middle || wide !== expected_wide)
        $fatal(1, "dynamic left demand oracle mismatch at %0d", step);
    end
    $display("dynamic left demand Verilog oracle passed (4096 vectors)");
    $finish;
  end
endmodule
