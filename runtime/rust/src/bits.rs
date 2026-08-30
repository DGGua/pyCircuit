//! Helpers for widths that do not match a native Rust integer.
//!
//! Common add/and/mux stay in generated code as `u8`/`bool` expressions.
//! These functions cover signed compare, arithmetic shift, and division.

pub const fn mask(width: u32) -> u64 {
    if width == 0 {
        0
    } else if width >= 64 {
        u64::MAX
    } else {
        (1u64 << width) - 1
    }
}

fn as_signed(val: u64, width: u32) -> i64 {
    if width == 0 {
        0
    } else if width >= 64 {
        val as i64
    } else {
        let shift = 64 - width;
        ((val as i64) << shift) >> shift
    }
}

pub fn slt_bits(a: u64, b: u64, width: u32) -> bool {
    as_signed(a & mask(width), width) < as_signed(b & mask(width), width)
}

pub fn ashr_bits(val: u64, width: u32, amount: u32) -> u64 {
    let v = val & mask(width);
    if amount == 0 {
        return v;
    }
    if amount >= width {
        return if width > 0 && (v >> (width - 1)) & 1 == 1 {
            mask(width)
        } else {
            0
        };
    }
    let s = as_signed(v, width) >> amount;
    (s as u64) & mask(width)
}

pub fn sext_bits(val: u64, in_width: u32, out_width: u32) -> u64 {
    (as_signed(val & mask(in_width), in_width) as u64) & mask(out_width)
}

pub fn udiv_bits(a: u64, b: u64, width: u32) -> u64 {
    let a = a & mask(width);
    let b = b & mask(width);
    if b == 0 {
        0
    } else {
        (a / b) & mask(width)
    }
}

pub fn urem_bits(a: u64, b: u64, width: u32) -> u64 {
    let a = a & mask(width);
    let b = b & mask(width);
    if b == 0 {
        0
    } else {
        (a % b) & mask(width)
    }
}

pub fn sdiv_bits(a: u64, b: u64, width: u32) -> u64 {
    let a = a & mask(width);
    let b = b & mask(width);
    if b == 0 {
        return 0;
    }
    let min = if width == 0 { 0 } else { 1u64 << (width - 1) };
    if a == min && b == mask(width) {
        return a;
    }
    let q = as_signed(a, width).wrapping_div(as_signed(b, width));
    (q as u64) & mask(width)
}

pub fn srem_bits(a: u64, b: u64, width: u32) -> u64 {
    let a = a & mask(width);
    let b = b & mask(width);
    if b == 0 {
        return 0;
    }
    let min = if width == 0 { 0 } else { 1u64 << (width - 1) };
    if a == min && b == mask(width) {
        return 0;
    }
    let r = as_signed(a, width).wrapping_rem(as_signed(b, width));
    (r as u64) & mask(width)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mask_and_slt() {
        assert_eq!(mask(4), 0xf);
        assert!(slt_bits(0xf, 0x1, 4));
        assert!(!slt_bits(0x1, 0xf, 4));
    }

    #[test]
    fn ashr_fills_ones() {
        assert_eq!(ashr_bits(0x80, 8, 4), 0xf8);
        assert_eq!(ashr_bits(0x10, 8, 2), 0x04);
    }

    #[test]
    fn sext_19() {
        assert_eq!(sext_bits(0x40000, 19, 32), 0xfffc0000);
    }
}
