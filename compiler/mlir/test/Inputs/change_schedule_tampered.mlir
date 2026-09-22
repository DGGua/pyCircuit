module {
  func.func @tampered(%arg0: i1) -> i1 attributes {
      pyc.change_schedule.summary = {
        edge_count = 1 : i64,
        node_count = 1 : i64,
        rank_count = 1 : i64,
        schema = "pyc.change_schedule.v1"
      }} {
    %0 = pyc.comb(%arg0) {
        pyc.change_schedule.fanout = [[0]],
        pyc.change_schedule.node = [0],
        pyc.change_schedule.rank = [0],
        pyc.change_schedule.slot = [0]
      } : (i1) -> i1 {
    ^bb0(%arg1: i1):
      %1 = pyc.not %arg1 : i1
      pyc.yield %1 : i1
    }
    return %0 : i1
  }
}
