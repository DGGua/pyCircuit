module attributes {pyc.frontend.contract = "pycircuit", pyc.top = @wire_driver_late_single} {
  func.func @wire_driver_late_single(%a: i8) -> i8 attributes {arg_names = ["a"], pyc.base = "wire_driver_late_single", pyc.inline = "false", pyc.kind = "module", pyc.params = "{}", pyc.struct.collections = "[]", pyc.struct.metrics = "{\22ast_node_count\22:0,\22collection_count\22:0,\22collection_instance_count\22:0,\22estimated_inline_cost\22:0,\22hardware_call_count\22:0,\22instance_count\22:0,\22loop_count\22:0,\22module_call_count\22:0,\22module_family_collection_count\22:0,\22repeat_pressure\22:0,\22repeated_body_clusters\22:[],\22source_loc\22:0,\22state_alloc_count\22:0,\22state_call_count\22:0}", pyc.value_param_types = [], pyc.value_params = [], result_names = ["y"]} {
    // A dead, unnamed, unread placeholder is legal and will be eliminated.
    %unused = pyc.wire : i8

    // The real source is intentionally defined after the wire consumer.  This
    // is valid late single-driver plumbing and must not be rejected merely
    // because textual operation order differs from dependency order.
    %w = pyc.wire : i8
    %y = pyc.not %w : i8
    %src = pyc.not %a : i8
    pyc.assign %w, %src : i8
    return %y : i8
  }
}
