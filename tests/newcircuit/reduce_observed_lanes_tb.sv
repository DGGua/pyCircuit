module reduce_observed_lanes_tb;
  reg [95:0] v;
  wire [7:0] or0, or2, and1, sum2;
  reg [7:0] expected_or0, expected_or2, expected_and1, expected_sum2;
  integer trial, row, col;
  top dut(.v(v), .or0(or0), .or2(or2), .and1(and1), .sum2(sum2));
  initial begin
    for (trial = 0; trial < 64; trial = trial + 1) begin
      expected_or0 = 0;
      expected_or2 = 0;
      expected_and1 = 8'hff;
      expected_sum2 = 0;
      for (row = 0; row < 3; row = row + 1)
        for (col = 0; col < 4; col = col + 1) begin
          v[(row * 4 + col) * 8 +: 8] = (trial * 37 + row * 19 + col * 53) & 8'hff;
          if (row == 0) expected_or0 = expected_or0 | v[(row * 4 + col) * 8 +: 8];
          if (row == 2) expected_or2 = expected_or2 | v[(row * 4 + col) * 8 +: 8];
          if (row == 1) expected_and1 = expected_and1 & v[(row * 4 + col) * 8 +: 8];
          if (col == 2) expected_sum2 = expected_sum2 + v[(row * 4 + col) * 8 +: 8];
        end
      #1;
      if (or0 !== expected_or0 || or2 !== expected_or2 ||
          and1 !== expected_and1 || sum2 !== expected_sum2)
        $fatal(1, "observed reduction lane");
    end
    $finish;
  end
endmodule
