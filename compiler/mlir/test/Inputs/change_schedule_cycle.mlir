module {
  func.func @bad() -> i1 {
    %wire = pyc.wire : i1
    %inverted = pyc.not %wire : i1
    pyc.assign %wire, %inverted : i1
    return %wire : i1
  }
}
