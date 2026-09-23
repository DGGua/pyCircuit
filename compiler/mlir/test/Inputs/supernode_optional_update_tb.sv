`timescale 1ns/1ps

module supernode_optional_update_tb;
  logic clk = 1'b0;
  logic rst = 1'b0;
  logic assert_ok = 1'b1;
  logic [7:0] a, mask, b, c;
  logic [129:0] wide, wide_mask, wide_bias;
  logic [7:0] vec_a0, vec_a1, vec_a2, vec_a3;
  logic [7:0] vec_mask0, vec_mask1, vec_mask2, vec_mask3;
  logic [7:0] vec_bias0, vec_bias1, vec_bias2, vec_bias3;
  wire [7:0] producer, result;
  wire [129:0] wide_out;
  wire [7:0] vec_out0, vec_out1, vec_out2, vec_out3, constant_out;
  wire reset_seen;

  supernode_optional_update dut (
    .clk(clk), .rst(rst), .a(a), .mask(mask), .b(b), .c(c),
    .assert_ok(assert_ok),
    .wide(wide), .wide_mask(wide_mask), .wide_bias(wide_bias),
    .vec_a0(vec_a0), .vec_a1(vec_a1), .vec_a2(vec_a2), .vec_a3(vec_a3),
    .vec_mask0(vec_mask0), .vec_mask1(vec_mask1),
    .vec_mask2(vec_mask2), .vec_mask3(vec_mask3),
    .vec_bias0(vec_bias0), .vec_bias1(vec_bias1),
    .vec_bias2(vec_bias2), .vec_bias3(vec_bias3),
    .producer(producer), .result(result), .wide_out(wide_out),
    .vec_out0(vec_out0), .vec_out1(vec_out1),
    .vec_out2(vec_out2), .vec_out3(vec_out3),
    .constant_out(constant_out), .reset_seen(reset_seen)
  );

  task automatic check(input bit cond, input string msg);
    if (!cond) begin
      $display("FAIL: %s", msg);
      $fatal(1);
    end
  endtask

  initial begin
    a = 8'h12;
    mask = 8'h00;
    b = 8'h03;
    c = 8'h05;
    assert_ok = 1'b1;
    wide = 130'h100000000000000020000000000001234;
    wide_mask = '0;
    wide_bias = 130'h000000000000000010000000000000055;
    vec_a0 = 8'h10;
    vec_a1 = 8'h20;
    vec_a2 = 8'h30;
    vec_a3 = 8'h40;
    vec_mask0 = 8'h00;
    vec_mask1 = 8'h00;
    vec_mask2 = 8'h00;
    vec_mask3 = 8'h00;
    vec_bias0 = 8'h01;
    vec_bias1 = 8'h02;
    vec_bias2 = 8'h03;
    vec_bias3 = 8'h04;
    #1;
    check(producer == 8'h00 && result == 8'h00, "initial scalar outputs");
    check(wide_out == wide_bias, "initial wide output");
    check(vec_out0 == 8'h01 && vec_out1 == 8'h02 &&
          vec_out2 == 8'h03 && vec_out3 == 8'h04, "initial vector outputs");
    check(constant_out == 8'h5a, "zero-input constant output");
    check(reset_seen == 1'b0, "reset_active low");

    a = 8'h34;
    #1;
    check(producer == 8'h00 && result == 8'h00, "masked producer input change");

    mask = 8'hff;
    #1;
    check(producer == 8'h34 && result == 8'h08, "fanout/reconvergence");

    vec_mask0 = 8'hff;
    vec_mask1 = 8'hff;
    vec_mask2 = 8'hff;
    vec_mask3 = 8'hff;
    wide_mask = '1;
    #1;
    check(vec_out0 == 8'h11 && vec_out1 == 8'h22 &&
          vec_out2 == 8'h33 && vec_out3 == 8'h44, "vector path");
    check(wide_out == (wide ^ wide_bias), "wide path");

    rst = 1'b1;
    #1;
    check(reset_seen == 1'b1, "reset_active high");

    $display("ok: Verilog partition semantics");
    $finish;
  end
endmodule
