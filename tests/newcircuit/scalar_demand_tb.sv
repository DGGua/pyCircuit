module tb;
  localparam integer CASE_ID = `CASE_ID;
  reg [31:0] a, b, c, d;
  wire [7:0] y0, y2, y3;
  wire [11:0] y1;
  wire [31:0] wide;
  reg [31:0] sum, inv, branch, bits;
  reg [31:0] e0, e1, e2, e3, ew;
  integer step;
  top dut(.a(a), .b(b), .c(c), .d(d), .y0(y0), .y1(y1),
          .y2(y2), .y3(y3), .wide(wide));
  initial begin
    for (step = 0; step < 4096; step = step + 1) begin
      a = step * 32'h9e3779b9 ^ 32'h80000000;
      b = step * 32'h85ebca6b + 32'hffffffff;
      c = step * 32'hc2b2ae35 ^ 32'haaaaaaaa;
      d = step * 32'h27d4eb2f + 32'h55555555;
      if ((step & 15) == 0) begin a = 0; b = 0; c = 0; d = 0; end
      if ((step & 15) == 1) begin a = '1; b = '1; c = '1; d = '1; end
      sum = a + b;
      inv = ~sum;
      e0 = 0; e1 = 0; e2 = 0; e3 = 0; ew = 0;
      if (CASE_ID == 0) e0 = inv & 15;
      else if (CASE_ID == 1) begin
        branch = (inv & c) ^ d;
        e0 = branch & 255;
        e1 = (branch >> 4) & 4095;
        e2 = ~(branch * a) & 255;
        e3 = (sum - d) & 255;
      end else if (CASE_ID == 2) begin
        bits = (a & b) & 255;
        e0 = (bits + c) & 255;
        e1 = (bits - d) & 255;
        e2 = bits; e3 = bits;
      end else if (CASE_ID == 3) begin
        e0 = inv & 255;
        e1 = (inv >> 8) & 4095;
        ew = inv;
      end else e0 = inv & 255;
      #1;
      if (y0 !== e0[7:0] || y1 !== e1[11:0] || y2 !== e2[7:0] ||
          y3 !== e3[7:0] || wide !== ew)
        $fatal(1, "scalar demand Verilog oracle mismatch at %0d", step);
    end
    $display("scalar demand Verilog oracle passed (4096 vectors)");
    $finish;
  end
endmodule
