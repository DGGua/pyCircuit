module array_probe_roots_tb;
  reg clk = 0, rst, en;
  reg [7:0] a, b;
  wire [7:0] y;
  reg [7:0] q = 0, qb = 0, expected;
  reg q0 = 0, q1 = 0;
  integer step;
  top dut(.*);
  initial begin
    for (step = 0; step < 256; step = step + 1) begin
      clk = 0; rst = step % 19 == 0; en = step % 5 != 0;
      a = step * 17 + 3; b = step * 29 + 5;
      #1;
      if (rst) q = 0; else if (en) q = a;
      if (rst) qb = 0; else if (en) qb = b;
      if (rst) begin q0 = 0; q1 = 0; end
      else if (en) begin q0 = a[0]; q1 = b[0]; end
      clk = 1;
      #1;
      expected = a + b;
      if (y !== b || dut.kept_sum !== expected || dut.kept_duplicate !== expected ||
          dut.kept_identity !== a || dut.kept_wire !== a ||
          dut.nested_keep !== (a ^ b) || dut.kept_state !== q ||
          dut.kept_vector[0] !== q || dut.kept_vector[1] !== qb ||
          dut.kept_flag0 !== q0 || dut.kept_flag1 !== q1 ||
          dut.kept_alias_flag0 !== q0 || dut.kept_alias_flag1 !== q1)
        $fatal(1, "retained observation mismatch step=%0d", step);
    end
    // Retention aliases must transmit four-state values without adding a
    // boolean conversion, assertion, mux condition or a hardware state cut.
    clk = 0; en = 0; rst = 0; a = 8'b10xz0101; b = 8'h33;
    #1;
    if (dut.kept_identity !== a || dut.kept_wire !== a || dut.kept_sum !== (a + b) ||
        dut.kept_duplicate !== (a + b) || dut.nested_keep !== (a ^ b))
      $fatal(1, "retained observation X/Z mismatch");
    $finish;
  end
endmodule
