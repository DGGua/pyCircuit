module {
  func.func @bad() -> i1 {
    %wire = pyc.wire : i1
    %zero = pyc.constant 0 : i1
    %one = pyc.constant 1 : i1
    pyc.assign %wire, %zero : i1
    pyc.assign %wire, %one : i1
    return %wire : i1
  }
}
