"""Data TCM window used by the scalar load/store path.

Coral maps DTCM at ``0x00010000`` for 32 KB. ``byte_mem`` reads combinationally,
so a load sees data in the same cycle the address is presented. Writes commit
on the clock edge. An unaligned word is one byte-addressed access; Coral splits
the same bytes across 16-byte rows instead of faulting, and the memory result
matches that.
"""

from __future__ import annotations

DTCM_BASE = 0x00010000
DTCM_BYTES = 32 * 1024


def _wire(value):
    """``byte_mem`` takes a Wire. Cycle-aware values keep that wire on ``_w``."""
    return value._w if hasattr(value, "_w") else value


def attach_dtcm(m, domain, raddr, wvalid, waddr, wdata, wstrb):
    """Bind a byte memory and return its 32-bit read data."""
    cd = domain.clock_domain
    return m.byte_mem(
        cd.clk,
        cd.rst,
        raddr=_wire(raddr),
        wvalid=_wire(wvalid),
        waddr=_wire(waddr),
        wdata=_wire(wdata),
        wstrb=_wire(wstrb),
        depth=DTCM_BYTES,
        name="dtcm",
    )
