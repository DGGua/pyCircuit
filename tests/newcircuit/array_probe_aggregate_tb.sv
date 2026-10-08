module array_probe_aggregate_tb;
  reg sel;
  reg [23:0] a, b;
  reg [7:0] x, z;
  wire [7:0] added, selected, created, broadcasted;
  reg [7:0] expected;
  integer step, lane;
  top dut(.*);
  initial begin
    for (step = 0; step < 256; step = step + 1) begin
      sel = step & 1;
      x = step; z = step + 23;
      for (lane = 0; lane < 3; lane = lane + 1) begin
        a[lane * 8 +: 8] = step * 7 + lane * 19;
        b[lane * 8 +: 8] = step * 31 + lane * 11;
      end
      #1;
      for (lane = 0; lane < 3; lane = lane + 1) begin
        expected = step * 38 + lane * 30;
        if (dut.aggregate_add[lane] !== expected ||
            dut.aggregate_mux[lane] !== (sel ? a[lane * 8 +: 8] : b[lane * 8 +: 8]))
          $fatal(1, "aggregate probe mismatch");
      end
      expected = step * 38 + 30;
      if (added !== expected || selected !== (sel ? a[15:8] : b[15:8]) ||
          created !== z || broadcasted !== x || dut.aggregate_create[0] !== x || dut.aggregate_create[1] !== z)
        $fatal(1, "aggregate output mismatch");
    end
    $finish;
  end
endmodule
