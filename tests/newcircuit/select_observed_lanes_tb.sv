module select_observed_lanes_tb;
  reg sel;
  reg [31:0] a, b;
  wire [7:0] lane0, lane3;
  integer step, lane;
  top dut(.sel(sel), .a(a), .b(b), .lane0(lane0), .lane3(lane3));
  initial begin
    for (step = 0; step < 64; step = step + 1) begin
      sel = step & 1;
      for (lane = 0; lane < 4; lane = lane + 1) begin
        a[lane * 8 +: 8] = (step + lane * 17) & 8'hff;
        b[lane * 8 +: 8] = (step * 3 + lane * 29) & 8'hff;
      end
      #1;
      if (lane0 !== (sel ? a[7:0] : b[7:0]) ||
          lane3 !== (sel ? a[31:24] : b[31:24]))
        $fatal(1, "selected vector lane");
    end
    $finish;
  end
endmodule
