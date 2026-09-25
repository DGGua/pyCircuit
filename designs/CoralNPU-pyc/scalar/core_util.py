"""Shared bit-concat helper for the scalar core."""

from __future__ import annotations


def cat_imm(*parts: object):
    """MSB-first concatenate. Local import avoids a cycle with ``pycircuit.cat``."""
    from pycircuit import cat

    return cat(*parts)
