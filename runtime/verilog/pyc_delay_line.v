// A fixed-depth, enable-aware delay line.
//
// This is structurally equivalent to DEPTH cascaded pyc_reg instances with a
// shared clock/reset/enable/init value.  Synthesis still sees the full set of
// flip-flops; the compact primitive primarily reduces generated-model and IR
// overhead.
module pyc_delay_line #(
  parameter integer WIDTH = 1,
  parameter integer DEPTH = 2
) (
  input  wire                 clk,
  input  wire                 rst,
  input  wire                 en,
  input  wire [WIDTH-1:0]     d,
  input  wire [WIDTH-1:0]     init,
  output wire [WIDTH-1:0]     q
);
  reg [WIDTH-1:0] stages [0:DEPTH-1];
  integer i;

  assign q = stages[DEPTH-1];

  always @(posedge clk) begin
    if (rst) begin
      for (i = 0; i < DEPTH; i = i + 1)
        stages[i] <= init;
    end else if (en) begin
      for (i = DEPTH-1; i > 0; i = i - 1)
        stages[i] <= stages[i-1];
      stages[0] <= d;
    end
  end
endmodule
