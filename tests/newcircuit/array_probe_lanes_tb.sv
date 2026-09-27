module array_probe_lanes_tb;
  reg clk = 0, rst, en;
  reg [47:0] d, init, a;
  reg [7:0] x, z;
  wire [7:0] lane01, duplicate01, lane12, created, broadcasted, state01;
  reg [7:0] q0 = 0, q1 = 0, expected0, expected1;
  integer cycle, row, col;
  top dut(.*);
  initial begin
    for (cycle = 0; cycle < 192; cycle = cycle + 1) begin
      clk = 0;
      rst = cycle == 0 || cycle % 19 == 0;
      en = cycle % 5 != 0;
      for (row = 0; row < 2; row = row + 1)
        for (col = 0; col < 3; col = col + 1) begin
          d[(row * 3 + col) * 8 +: 8] = cycle * 31 + row * 47 + col * 13;
          init[(row * 3 + col) * 8 +: 8] = cycle * 7 + row * 43 + col * 29;
          a[(row * 3 + col) * 8 +: 8] = cycle * 17 + row * 71 + col * 11;
        end
      x = cycle * 41;
      z = cycle * 59 + 3;
      #1;
      if (rst) begin
        q0 = cycle * 7 + 29;
        q1 = cycle * 7 + 43 + 58;
      end else if (en) begin
        q0 = cycle * 31 + 13;
        q1 = cycle * 31 + 47 + 26;
      end
      clk = 1;
      #1;
      expected0 = q0 + cycle * 17 + 11;
      expected1 = q1 + cycle * 17 + 71 + 22;
      if (state01 !== q0 || dut.state_probe !== q0 || lane01 !== expected0 || duplicate01 !== expected0 || lane12 !== expected1 ||
          created !== z || broadcasted !== x ||
          dut.sum_probe !== expected0 || dut.duplicate_probe !== expected0 ||
          dut.created_probe !== z || dut.broadcast_probe !== x)
        $fatal(1, "array lane/probe mismatch cycle=%0d", cycle);
    end
    $finish;
  end
endmodule
