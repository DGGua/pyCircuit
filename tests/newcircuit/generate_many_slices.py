"""Generate designs with more than 64 readers of one wide value."""

import json
from pathlib import Path
import sys


def generate(kind: str, output: Path) -> None:
    state = kind == "register"
    metrics = {
        "source_loc": 0,
        "ast_node_count": 0,
        "hardware_call_count": 0,
        "loop_count": 0,
        "module_call_count": 0,
        "state_call_count": int(state),
        "estimated_inline_cost": 0,
        "instance_count": 0,
        "state_alloc_count": int(state),
        "collection_count": 0,
        "collection_instance_count": 0,
        "module_family_collection_count": 0,
        "repeated_body_clusters": [],
    }
    arguments = (
        "%clk: !pyc.clock, %rst: !pyc.reset, %a: i65"
        if state else "%a: i65, %b: i65"
    )
    names = '"clk", "rst", "a"' if state else '"a", "b"'
    lines = [
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {',
        f"  func.func @top({arguments}) -> vector<65xi1> attributes "
        f'{{arg_names = [{names}], result_names = ["y"], pyc.kind = "module", '
        f'pyc.inline = "false", pyc.params = "{{}}", pyc.base = "top", '
        f"pyc.struct.metrics = {json.dumps(json.dumps(metrics, separators=(',', ':')))}, "
        'pyc.struct.collections = "[]"} {',
    ]
    if state:
        lines += [
            "    %en = pyc.constant 1 : i1",
            "    %init = pyc.constant 0 : i65",
            "    %whole = pyc.reg %clk, %rst, %en, %a, %init : i65",
        ]
    else:
        lines.append("    %whole = pyc.and %a, %b : i65, i65 -> i65")
    for bit in range(65):
        lines.append(
            f"    %s{bit} = pyc.extract %whole "
            f"{{lsb = {bit} : i64, msb = {bit} : i64}} : i65 -> i1"
        )
    values = ", ".join(f"%s{bit}" for bit in range(65))
    types = ", ".join("i1" for _ in range(65))
    lines += [
        f"    %vec = pyc.v_create({values}) : ({types}) -> vector<65xi1>",
        "    return %vec : vector<65xi1>",
        "  }",
        "}",
    ]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_vector_lanes(output: Path) -> None:
    metrics = {
        "source_loc": 0, "ast_node_count": 0, "hardware_call_count": 0,
        "loop_count": 0, "module_call_count": 0, "state_call_count": 0,
        "estimated_inline_cost": 0, "instance_count": 0,
        "state_alloc_count": 0, "collection_count": 0,
        "collection_instance_count": 0, "module_family_collection_count": 0,
        "repeated_body_clusters": [],
    }
    lines = [
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {',
        '  func.func @top(%a: vector<65xi8>, %b: vector<65xi8>) '
        '-> vector<65xi1> attributes {arg_names = ["a", "b"], '
        'result_names = ["y"], pyc.kind = "module", pyc.inline = "false", '
        'pyc.params = "{}", pyc.base = "top", '
        f"pyc.struct.metrics = {json.dumps(json.dumps(metrics, separators=(',', ':')))}, "
        'pyc.struct.collections = "[]"} {',
        '    %whole = pyc.udiv %a, %b : vector<65xi8>, vector<65xi8> '
        '-> vector<65xi8>',
        '    %threshold = pyc.constant 3 : i8',
    ]
    for lane in range(65):
        lines += [
            f'    %d{lane} = pyc.v_get %whole[{lane}] : vector<65xi8> -> i8',
            f'    %c{lane} = pyc.ult %d{lane}, %threshold : i8, i8 -> i1',
        ]
    values = ", ".join(f"%c{lane}" for lane in range(65))
    types = ", ".join("i1" for _ in range(65))
    lines += [
        f"    %vec = pyc.v_create({values}) : ({types}) -> vector<65xi1>",
        "    return %vec : vector<65xi1>",
        "  }", "}",
    ]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_vector_register(output: Path) -> None:
    metrics = {
        "source_loc": 0, "ast_node_count": 0, "hardware_call_count": 0,
        "loop_count": 0, "module_call_count": 0, "state_call_count": 1,
        "estimated_inline_cost": 0, "instance_count": 0,
        "state_alloc_count": 1, "collection_count": 0,
        "collection_instance_count": 0, "module_family_collection_count": 0,
        "repeated_body_clusters": [],
    }
    lines = [
        'module attributes {pyc.top = @top, pyc.frontend.contract = "pycircuit"} {',
        '  func.func @top(%clk: !pyc.clock, %rst: !pyc.reset, '
        '%d: vector<65xi8>) -> vector<65xi8> attributes '
        '{arg_names = ["clk", "rst", "d"], result_names = ["y"], '
        'pyc.kind = "module", pyc.inline = "false", pyc.params = "{}", '
        'pyc.base = "top", '
        f"pyc.struct.metrics = {json.dumps(json.dumps(metrics, separators=(',', ':')))}, "
        'pyc.struct.collections = "[]"} {',
        '    %zero = pyc.constant 0 : i8',
        '    %en = pyc.constant 1 : i1',
        '    %init = pyc.v_broadcast %zero to 65 : i8 -> vector<65xi8>',
        '    %q = pyc.reg %clk, %rst, %en, %d, %init : vector<65xi8>',
    ]
    for lane in range(65):
        lines.append(
            f'    %r{lane} = pyc.v_get %q[{lane}] : vector<65xi8> -> i8'
        )
    values = ", ".join(f"%r{lane}" for lane in reversed(range(65)))
    types = ", ".join("i8" for _ in range(65))
    lines += [
        f"    %vec = pyc.v_create({values}) : ({types}) -> vector<65xi8>",
        "    return %vec : vector<65xi8>",
        "  }", "}",
    ]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    directory = Path(sys.argv[1])
    generate("bitwise", directory / "many_bitwise_slices.pyc")
    generate("register", directory / "many_register_slices.pyc")
    generate_vector_lanes(directory / "many_vector_lanes.pyc")
    generate_vector_register(directory / "many_vector_register.pyc")
