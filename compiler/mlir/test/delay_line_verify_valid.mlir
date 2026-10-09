// A valid fixed-latency sequential delay line.
module {
  func.func @delay_line_valid(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    %q = pyc.delay_line %clk, %rst, %en, %in, %init {depth = 4 : i64} : i8
    func.return %q : i8
  }

  func.func @delay_line_vector_valid(%clk: !pyc.clock, %rst: !pyc.reset,
                                     %in: vector<4xi8>, %init: vector<4xi8>) -> vector<4xi8> {
    %en = pyc.constant 1 : i1
    %q = pyc.delay_line %clk, %rst, %en, %in, %init {depth = 2 : i64} : vector<4xi8>
    func.return %q : vector<4xi8>
  }
}
