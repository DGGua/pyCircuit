module multi_reader_immediate_shift_tb;
  reg [127:0] a;
  wire [7:0] sh_low, sh_mid, ls_low, ls_mid, as_low, as_mid;
  wire [7:0] lsfill_low, lsfill_mid, fill_low, fill_mid;
  wire [7:0] zero_low, zero_mid, sign_low, sign_mid;
  wire [7:0] dyn_sh_low, dyn_sh_mid, dyn_ls_low, dyn_ls_mid, dyn_as_low, dyn_as_mid;
  top dut(.a(a), .sh_low(sh_low), .sh_mid(sh_mid), .ls_low(ls_low), .ls_mid(ls_mid),
          .as_low(as_low), .as_mid(as_mid), .lsfill_low(lsfill_low),
          .lsfill_mid(lsfill_mid), .fill_low(fill_low), .fill_mid(fill_mid),
          .zero_low(zero_low), .zero_mid(zero_mid),
          .sign_low(sign_low), .sign_mid(sign_mid),
          .dyn_sh_low(dyn_sh_low), .dyn_sh_mid(dyn_sh_mid),
          .dyn_ls_low(dyn_ls_low), .dyn_ls_mid(dyn_ls_mid),
          .dyn_as_low(dyn_as_low), .dyn_as_mid(dyn_as_mid));
  reg [127:0] expected;
  task check;
    begin
      #1;
      expected = a << 7;
      if (sh_low !== expected[7:0] || sh_mid !== expected[39:32]) $fatal(1, "shli");
      expected = a >> 5;
      if (ls_low !== expected[7:0] || ls_mid !== expected[39:32]) $fatal(1, "lshri");
      expected = $signed(a) >>> 6;
      if (as_low !== expected[7:0] || as_mid !== expected[39:32]) $fatal(1, "ashri");
      expected = a >> 120;
      if (lsfill_low !== expected[7:0] || lsfill_mid !== expected[39:32]) $fatal(1, "lshri fill");
      expected = $signed(a) >>> 120;
      if (fill_low !== expected[7:0] || fill_mid !== expected[39:32]) $fatal(1, "ashri fill");
      if (zero_low !== 0 || zero_mid !== 0) $fatal(1, "lshri all fill");
      if (sign_low !== (a[127] ? 8'hff : 8'h00) ||
          sign_mid !== (a[127] ? 8'hff : 8'h00)) $fatal(1, "ashri all fill");
      expected = a << 3;
      if (dyn_sh_low !== expected[7:0] || dyn_sh_mid !== expected[39:32]) $fatal(1, "dynamic shl");
      expected = a >> 3;
      if (dyn_ls_low !== expected[7:0] || dyn_ls_mid !== expected[39:32]) $fatal(1, "dynamic lshr");
      expected = $signed(a) >>> 3;
      if (dyn_as_low !== expected[7:0] || dyn_as_mid !== expected[39:32]) $fatal(1, "dynamic ashr");
    end
  endtask
  initial begin
    a = 128'h123456789abcdef0fedcba9876543210; check();
    a = 128'hfedcba9876543210ffffffffffffffff; check();
    a = 128'h80000000000000000000000000000000; check();
    $finish;
  end
endmodule
