"""Structured record (bus) support for pyCircuit cycle-aware modules.

A :class:`RecordSpec` defines a named set of scalar and indexed-array
fields.  Helper functions create input/output ports, entry registers,
muxes, and next-state values from a spec, so modules can pass structured
data through pyCircuit's flat-port infrastructure with a consistent API.

Port naming convention
----------------------
For a spec with prefix ``"in"``:

* Scalar field ``engine_kind``  port ``in_engine_kind_0``
* Array  field ``tile_reg_inputs`` / sub ``tile_type`` / lane 0
  port ``in_tile_reg_inputs_tile_type_0``
* Array single-value ``output_replaced_tile_tags`` / lane 0
  port ``in_output_replaced_tile_tags_0``
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Iterator, Union

from .v5 import cas, mux, wire_of


# ── Spec types ──────────────────────────────────────────────────────


@dataclass(frozen=True)
class RecordField:
    name: str
    width: int
    reset_value: int = 0

    def __post_init__(self) -> None:
        if int(self.width) <= 0:
            raise ValueError(f"RecordField {self.name!r}: width must be > 0")


@dataclass(frozen=True)
class RecordArray:
    name: str
    lanes: int
    fields: tuple[RecordField, ...]

    def __post_init__(self) -> None:
        if int(self.lanes) <= 0:
            raise ValueError(f"RecordArray {self.name!r}: lanes must be > 0")
        if not self.fields:
            raise ValueError(f"RecordArray {self.name!r}: must have >= 1 field")


@dataclass(frozen=True)
class RecordSpec:
    name: str
    scalars: tuple[RecordField, ...]
    arrays: tuple[RecordArray, ...] = ()

    def entries(self) -> Iterator[tuple[str, str, int, int]]:
        r"""Yield ``(key, port_suffix, width, reset_value)`` for every field.

        *key* is the internal dict key; *port_suffix* is appended to the
        caller-supplied prefix to form the full pyCircuit port name.
        """
        for f in self.scalars:
            yield (f.name, f"{f.name}_0", f.width, f.reset_value)
        for arr in self.arrays:
            for lane in range(arr.lanes):
                for f in arr.fields:
                    if f.name:
                        k = f"{arr.name}_{f.name}_{lane}"
                    else:
                        k = f"{arr.name}_{lane}"
                    yield (k, k, f.width, f.reset_value)

    def all_keys(self) -> list[str]:
        return [e[0] for e in self.entries()]

    def width_for(self, key: str) -> int:
        for k, _, w, _ in self.entries():
            if k == key:
                return w
        raise KeyError(f"RecordSpec {self.name!r}: unknown key {key!r}")

    def reset_for(self, key: str) -> int:
        for k, _, _, r in self.entries():
            if k == key:
                return r
        raise KeyError(f"RecordSpec {self.name!r}: unknown key {key!r}")


# ── Record value handle ─────────────────────────────────────────────


class Record(Mapping):
    """A dict-like collection of signals following a :class:`RecordSpec`.

    Behaves as a ``dict[str, signal]``: supports ``__getitem__``,
    ``__setitem__``, iteration, ``len()``, ``in``, and ``dict()``.
    """

    __slots__ = ("spec", "_signals")

    def __init__(self, spec: RecordSpec, signals: dict[str, Any]):
        self.spec = spec
        self._signals = signals

    def __getitem__(self, key: str) -> Any:
        return self._signals[key]

    def __setitem__(self, key: str, value: Any) -> None:
        self._signals[key] = value

    def __iter__(self) -> Iterator[str]:
        return iter(self._signals)

    def __len__(self) -> int:
        return len(self._signals)


# ── Convenience type alias ──────────────────────────────────────────

SignalMap = Union[Record, dict]


# ── Helper functions ────────────────────────────────────────────────


def _port(io, full_name, m, domain, width):
    if io is not None and full_name in io:
        return io[full_name]
    return cas(domain, m.input(full_name, width=width), cycle=0)


def record_input(m, domain, spec: RecordSpec, prefix: str,
                 inputs: dict | None = None) -> Record:
    """Declare input ports for a full record and return a :class:`Record`."""
    sigs: dict[str, Any] = {}
    for key, psuf, width, _ in spec.entries():
        sigs[key] = _port(inputs, f"{prefix}_{psuf}", m, domain, width)
    return Record(spec, sigs)


def record_output(m, spec: RecordSpec, prefix: str,
                  signals: SignalMap) -> None:
    """Emit ``m.output()`` calls for every field in *signals*."""
    for key, psuf, _, _ in spec.entries():
        m.output(f"{prefix}_{psuf}", wire_of(signals[key]))


def record_outputs_dict(spec: RecordSpec, prefix: str,
                        signals: SignalMap) -> dict:
    """Build a ``{port_name: signal}`` dict without calling ``m.output()``."""
    return {f"{prefix}_{psuf}": signals[key]
            for key, psuf, _, _ in spec.entries()}


def record_state(domain, spec: RecordSpec, entry_idx: int,
                 prefix: str) -> Record:
    """Create per-entry state registers for all fields."""
    sigs: dict[str, Any] = {}
    for key, _, width, reset in spec.entries():
        sigs[key] = domain.signal(
            width=width, reset_value=reset,
            name=f"{prefix}_e{entry_idx}_{key}")
    return Record(spec, sigs)


def record_mux(spec: RecordSpec, records: list, idx_signal,
               m, domain) -> Record:
    """Mux between multiple record instances selected by *idx_signal*."""
    n = len(records)
    idx_w = max(1, (n - 1).bit_length()) if n > 1 else 1
    sigs: dict[str, Any] = {}
    for key in spec.all_keys():
        sig = records[0][key]
        for i in range(1, n):
            const_i = cas(domain, m.const(i, width=idx_w), cycle=0)
            sig = mux(idx_signal == const_i, records[i][key], sig)
        sigs[key] = sig
    return Record(spec, sigs)


def record_next(spec: RecordSpec, entry: SignalMap, src: SignalMap,
                write_en, m, domain) -> Record:
    """Compute next-state: *src* when *write_en*, else *entry*."""
    sigs: dict[str, Any] = {}
    for key in spec.all_keys():
        sigs[key] = mux(write_en, src[key], entry[key])
    return Record(spec, sigs)
