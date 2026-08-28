// Verifier rejection: a delay line represents at least two register stages.
module {
  func.func @depth_one(%clk: !pyc.clock, %rst: !pyc.reset, %in: i8) -> i8 {
    %en = pyc.constant 1 : i1
    %init = pyc.constant 0 : i8
    // expected-error@+1 {{'pyc.delay_line' op depth must be > 1}}
    %q = pyc.delay_line %clk, %rst, %en, %in, %init {depth = 1 : i64} : i8
    func.return %q : i8
  }
}
