module reduce_register_lanes_tb;
  reg clk=0, rst=0, en=1;
  reg [95:0] d=0, init=0;
  wire [7:0] sum2;
  integer row;
  top dut(.clk(clk), .rst(rst), .en(en), .d(d), .init(init), .sum2(sum2));
  initial begin
    for (row = 0; row < 3; row = row + 1) begin
      init[(row * 4 + 2) * 8 +: 8] = row + 1;
      d[(row * 4 + 2) * 8 +: 8] = row + 7;
    end
    rst=1; #1; clk=1; #1;
    if (sum2 !== 8'd6) $fatal(1, "reset sum");
    rst=0; clk=0; #1; clk=1; #1;
    if (sum2 !== 8'd24) $fatal(1, "data sum");
    d[7:0]=8'd99; clk=0; #1; clk=1; #1;
    if (sum2 !== 8'd24) $fatal(1, "unobserved lane");
    $finish;
  end
endmodule
