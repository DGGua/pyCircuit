module tb;
  localparam integer AMOUNT_WIDTH = `AMOUNT_WIDTH;
  reg [127:0] a;
  reg [AMOUNT_WIDTH-1:0] amount;
  wire [127:0] left, logical, arithmetic;
  wire [3:0] low;
  reg [63:0] lo, hi, amount_lo, amount_hi;
  reg [127:0] expected_left, expected_logical, expected_arithmetic;
  reg [63:0] amounts [0:15];
  integer step, unknown_case;
  top dut(.*);
  initial begin
    amounts[0] = 0; amounts[1] = 1; amounts[2] = 31; amounts[3] = 32;
    amounts[4] = 63; amounts[5] = 64; amounts[6] = 127; amounts[7] = 128;
    amounts[8] = 255; amounts[9] = 64'h100000000; amounts[10] = 64'h100000001;
    amounts[11] = 64'h8000000000000000; amounts[12] = 64'hffffffffffffffff;
    amounts[13] = 0; amounts[14] = 1; amounts[15] = 127;
    for (step = 0; step < 4096; step = step + 1) begin
      lo = step * 64'h9e3779b97f4a7c15 ^ 64'hffffffffffffffff;
      hi = step * 64'hd6e8feb86659fd93 ^ 64'h8000000000000000;
      a = {hi, lo};
      amount_lo = amounts[step & 15];
      amount_hi = AMOUNT_WIDTH == 128 && (step & 15) >= 13
                      ? (64'b1 << ((step & 15) == 15 ? 63 : 0)) : 0;
      amount = {amount_hi, amount_lo};
      // Compare the entire amount before shifting. Some Icarus versions
      // truncate procedural shift counts to 32 bits, including 1 << 32.
      if (amount >= 128) begin
        expected_left = 0;
        expected_logical = 0;
        expected_arithmetic = {128{a[127]}};
      end else begin
        expected_left = a << amount[6:0];
        expected_logical = a >> amount[6:0];
        expected_arithmetic = $signed(a) >>> amount[6:0];
      end
      #1;
      if (left !== expected_left || logical !== expected_logical ||
          arithmetic !== expected_arithmetic || low !== expected_left[3:0])
        $fatal(1, "wide shift amount oracle mismatch at %0d", step);
    end
    // Any unknown bit of a shift amount makes the whole result unknown,
    // even when the known low bits would already overshift the input.
    for (unknown_case = 0; unknown_case < 12; unknown_case = unknown_case + 1) begin
      a = unknown_case < 6 ? 128'h80000000000000000123456789abcdef : 0;
      amount = 0;
      case (unknown_case % 6)
        0: begin amount = 128; amount[AMOUNT_WIDTH-1] = 1'bx; end
        1: begin amount = 128; amount[AMOUNT_WIDTH-1] = 1'bz; end
        2: amount[0] = 1'bx;
        3: amount[0] = 1'bz;
        4: begin amount = 1; amount[AMOUNT_WIDTH-1] = 1'bx; end
        5: begin amount = 1; amount[AMOUNT_WIDTH-1] = 1'bz; end
      endcase
      #1;
      if (left !== {128{1'bx}} || logical !== {128{1'bx}} ||
          arithmetic !== {128{1'bx}} || low !== 4'bxxxx)
        $fatal(1, "wide shift unknown amount oracle mismatch at %0d", unknown_case);
    end
    $display("wide shift amount Verilog oracle passed (4096 vectors, 12 X/Z vectors)");
    $finish;
  end
endmodule
