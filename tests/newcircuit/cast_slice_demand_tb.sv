module cast_slice_demand_tb;
  reg [15:0] a;
  wire [7:0] zlo, zcross, zhigh, slo, scross, shigh;
  wire [3:0] tlo, thi;
  integer trial;
  top dut(.a(a), .zlo(zlo), .zcross(zcross), .zhigh(zhigh),
          .slo(slo), .scross(scross), .shigh(shigh), .tlo(tlo), .thi(thi));
  initial begin
    for (trial = 0; trial < 256; trial = trial + 1) begin
      a = (trial * 16'h7715) ^ 16'hac37;
      #1;
      if (zlo !== a[11:4] || zcross !== {4'b0, a[15:12]} ||
          zhigh !== 8'b0 || slo !== a[11:4] ||
          scross !== {{4{a[15]}}, a[15:12]} ||
          shigh !== {8{a[15]}} || tlo !== a[3:0] ||
          thi !== a[7:4])
        $fatal(1, "cast slice demand");
    end
    $finish;
  end
endmodule
