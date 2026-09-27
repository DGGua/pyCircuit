module array_probe_memory_tb;
  reg clk = 0, rst, ren0, ren1, wvalid;
  reg [1:0] raddr0, raddr1, waddr;
  reg [31:0] wdata;
  reg [3:0] wstrb;
  wire [7:0] low16, low32a, low32b;
  reg [15:0] m16 [0:3];
  reg [31:0] m32 [0:3];
  reg [7:0] read16 = 0, read32a = 0, read32b = 0;
  integer step, addr, byte_lane;
  top dut(.*);
  initial begin
    for (addr = 0; addr < 4; addr = addr + 1) begin m16[addr] = 0; m32[addr] = 0; end
    for (step = 0; step < 256; step = step + 1) begin
      clk = 0; rst = step % 19 == 0; ren0 = step % 3 != 0; ren1 = step % 5 != 0;
      wvalid = step % 7 != 0; raddr0 = step % 4; raddr1 = (step + 1) % 4; waddr = raddr0;
      wdata = (((step * 32'h01010101) ^ 32'h89abcdef) & 32'hffffff00) | 32'h5a;
      wstrb = step < 8 ? 15 : (1 << (step % 4));
      #1;
      if (rst) begin read16 = 0; read32a = 0; read32b = 0; end
      else begin
        if (ren0) begin read16 = m16[raddr0][7:0]; read32a = m32[raddr0][7:0]; end
        if (ren1) read32b = m32[raddr1][7:0];
        if (wvalid)
          for (byte_lane = 0; byte_lane < 4; byte_lane = byte_lane + 1)
            if (wstrb[byte_lane]) begin
              m32[waddr][byte_lane * 8 +: 8] = wdata[byte_lane * 8 +: 8];
              if (byte_lane < 2) m16[waddr][byte_lane * 8 +: 8] = wdata[byte_lane * 8 +: 8];
            end
      end
      clk = 1;
      #1;
      if (low16 !== read16 || low32a !== read32a || low32b !== read32b)
        $fatal(1, "memory old-data/hold/reset mismatch");
      for (addr = 0; addr < 4; addr = addr + 1)
        if (dut.mem16.mem[addr] !== m16[addr] || dut.mem32.mem[addr] !== m32[addr])
          $fatal(1, "memory observed high-byte mismatch");
    end
    $finish;
  end
endmodule
