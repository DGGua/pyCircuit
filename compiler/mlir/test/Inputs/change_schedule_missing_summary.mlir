module {
  func.func private @child(i1) -> i1
  func.func @top(%arg0: i1) -> i1 {
    %0 = pyc.instance %arg0 {callee = @child, name = "u_child"} :
        (i1) -> i1
    return %0 : i1
  }
}
