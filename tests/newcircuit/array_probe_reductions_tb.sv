module array_probe_reductions_tb;
  reg [95:0] v;
  reg [31:0] r;
  wire [7:0] or0, and1, sum2, row2, col1;
  reg [7:0] expected_or, expected_and, expected_sum, value;
  integer step, row, col;
  top dut(.*);
  initial begin
    for (step = 0; step < 256; step = step + 1) begin
      expected_or = 0; expected_and = 255; expected_sum = 0;
      for (row = 0; row < 3; row = row + 1)
        for (col = 0; col < 4; col = col + 1) begin
          value = step * 37 + row * 73 + col * 29;
          v[(row * 4 + col) * 8 +: 8] = value;
          if (row == 0) expected_or = expected_or | value;
          if (row == 1) expected_and = expected_and & value;
          if (col == 2) expected_sum = expected_sum + value;
        end
      for (col = 0; col < 4; col = col + 1)
        r[col * 8 +: 8] = step * 17 + col * 11;
      #1;
      if (or0 !== expected_or || and1 !== expected_and || sum2 !== expected_sum ||
          row2 !== r[23:16] || col1 !== r[23:16] ||
          dut.or_probe !== expected_or || dut.and_probe !== expected_and ||
          dut.sum_probe !== expected_sum) $fatal(1, "reduction probe mismatch");
      for (col = 0; col < 4; col = col + 1)
        if (dut.row_probe[col] !== r[col * 8 +: 8]) $fatal(1, "row probe mismatch");
      for (col = 0; col < 2; col = col + 1)
        if (dut.column_probe[col] !== r[23:16]) $fatal(1, "column probe mismatch");
    end
    $finish;
  end
endmodule
