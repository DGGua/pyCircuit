module gapped_mux_slices_tb;
  reg sel;
  reg [23:0] a, b;
  wire [7:0] lo, overlap;
  wire [3:0] hi;
  reg [23:0] selected;
  integer trial;
  top dut(.sel(sel), .a(a), .b(b), .lo(lo), .overlap(overlap), .hi(hi));
  initial begin
    for (trial = 0; trial < 128; trial = trial + 1) begin
      sel = trial & 1;
      a = (trial * 24'h157bcd) ^ 24'hac1234;
      b = (trial * 24'h791135) ^ 24'hce5678;
      selected = sel ? a : b;
      #1;
      if (lo !== selected[7:0] || overlap !== selected[11:4] ||
          hi !== selected[23:20])
        $fatal(1, "gapped mux slices");
    end
    $finish;
  end
endmodule
