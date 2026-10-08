module multi_reader_mux_tb;
  reg sel;
  reg [127:0] a, b;
  wire [7:0] low, mid;
  top dut(.sel(sel), .a(a), .b(b), .low(low), .mid(mid));
  reg [127:0] expected;
  task check;
    begin
      #1;
      expected = sel ? a : b;
      if (low !== expected[7:0] || mid !== expected[39:32])
        $fatal(1, "mux");
    end
  endtask
  initial begin
    a = 128'h123456789abcdef0fedcba9876543210;
    b = 128'h0f1e2d3c4b5a69788776655443322110;
    sel = 0; check();
    sel = 1; check();
    a = 128'hffffffffffffffffffffffffffffffff;
    b = 128'h000000000000000000000000ffffffff;
    sel = 0; check();
    sel = 1; check();
    $finish;
  end
endmodule
