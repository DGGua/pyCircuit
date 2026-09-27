module array_probe_slp_tb;
  reg [7:0] a, b, c, d;
  reg s, t;
  wire [15:0] xors, nots, muxes;
  wire [1:0] equals;
  integer step;
  top dut(.*);
  initial begin
    for (step = 0; step < 256; step = step + 1) begin
      a = step; b = step * 7; c = step * 31; d = step * 11;
      s = step & 1; t = (step & 2) != 0;
      #1;
      if (dut.xor0 !== (a ^ b) || dut.xor1 !== (c ^ d) ||
          dut.not0 !== ~a || dut.not1 !== ~c ||
          dut.mux0 !== (s ? a : b) || dut.mux1 !== (t ? c : d) ||
          dut.equal_pair[0] !== (a == b) || dut.equal_pair[1] !== (c == d) ||
          xors !== {c ^ d, a ^ b} || nots !== {~c, ~a} ||
          muxes !== {t ? c : d, s ? a : b} || equals !== {c == d, a == b})
        $fatal(1, "SLP observation mismatch");
    end
    $finish;
  end
endmodule
