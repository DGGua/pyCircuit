// COMB-LABEL: func.func @observation_demand_comb
// COMB: pyc.lazy_probe_wires
// COMB-SAME: name = "ready_state"
// COMB-SAME: name = "ready_alias"
// COMB: pyc.name = "ready_state"
// COMB-SAME: pyc.observe_lazy
// COMB: pyc.name = "ready_alias"
// COMB-SAME: pyc.observe_lazy

module {
  func.func @observation_demand_comb(%a: i8, %b: i8) -> i8 {
    %y = pyc.and %a, %b {pyc.name = "ready_state"} : i8, i8 -> i8
    %n = pyc.alias %y {pyc.name = "ready_alias"} : i8
    func.return %y : i8
  }
}
