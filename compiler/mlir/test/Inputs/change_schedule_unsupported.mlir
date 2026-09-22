module {
  func.func @bad(%arg0: i1, %arg1: i1) -> i1 {
    %0 = arith.addi %arg0, %arg1 : i1
    return %0 : i1
  }
}
