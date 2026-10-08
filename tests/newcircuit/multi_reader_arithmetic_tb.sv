module multi_reader_arithmetic_tb;
  reg [127:0] a, b;
  wire [7:0] add_low, add_mid, sub_low, sub_mid, mul_low, mul_mid;
  top dut(.a(a), .b(b), .add_low(add_low), .add_mid(add_mid),
          .sub_low(sub_low), .sub_mid(sub_mid),
          .mul_low(mul_low), .mul_mid(mul_mid));
  reg [127:0] expected;
  task check;
    begin
      #1;
      expected = a + b;
      if (add_low !== expected[7:0] || add_mid !== expected[39:32]) $fatal(1, "add");
      expected = a - b;
      if (sub_low !== expected[7:0] || sub_mid !== expected[39:32]) $fatal(1, "sub");
      expected = a * b;
      if (mul_low !== expected[7:0] || mul_mid !== expected[39:32]) $fatal(1, "mul");
    end
  endtask
  initial begin
    a = 128'h123456789abcdef0fedcba9876543210;
    b = 128'h0f1e2d3c4b5a69788776655443322110; check();
    a = 128'hffffffffffffffffffffffffffffffff;
    b = 128'h000000000000000000000000ffffffff; check();
    a = 128'h00000000000000000000000100000000;
    b = 128'h000000000000000000000000ffffffff; check();
    $finish;
  end
endmodule
