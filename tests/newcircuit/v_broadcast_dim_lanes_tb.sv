module v_broadcast_dim_lanes_tb;
  reg [15:0] a;
  wire [7:0] row_item;
  wire [23:0] col_row;
  top dut(.a(a), .row_item(row_item), .col_row(col_row));
  task check;
    begin
      #1;
      if (row_item !== a[15:8] || col_row !== {3{a[15:8]}})
        $fatal(1, "broadcast lane");
    end
  endtask
  initial begin
    a = 16'h0703; check();
    a = 16'hff00; check();
    a = 16'h00ff; check();
    $finish;
  end
endmodule
