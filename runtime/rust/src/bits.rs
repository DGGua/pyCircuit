//! Fixed-width bit vector for the Rust emitter subset.
//!
//! Mirrors `runtime/cpp/pyc_bits.hpp` `Bits<Width>` / `Wire<Width>` for widths
//! 1..=64 (one `u64` plus a mask). Wider or vector types are rejected at emit
//! time so this crate can stay dependency-free and simple.

use core::ops::{Add, BitAnd, BitOr, BitXor, Mul, Not, Sub};

/// Unsigned bit vector of compile-time width `W` (1..=64).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Wire<const W: u32> {
    bits: u64,
}

impl<const W: u32> Wire<W> {
    /// Mask of the live bits. `W == 64` uses all 64 bits.
    pub const fn mask() -> u64 {
        debug_assert!(W >= 1 && W <= 64);
        if W >= 64 {
            u64::MAX
        } else {
            (1u64 << W) - 1
        }
    }

    pub const fn new(value: u64) -> Self {
        Self {
            bits: value & Self::mask(),
        }
    }

    pub const fn value(self) -> u64 {
        self.bits
    }

    pub const fn to_bool(self) -> bool {
        (self.bits & 1) != 0
    }

    pub const fn bit(self, i: u32) -> bool {
        if i >= W {
            false
        } else {
            ((self.bits >> i) & 1) != 0
        }
    }

    pub const fn ones() -> Self {
        Self { bits: Self::mask() }
    }

    const fn as_signed(self) -> i64 {
        if W == 64 {
            self.bits as i64
        } else {
            let sign_bit = 1u64 << (W - 1);
            if self.bits & sign_bit != 0 {
                (self.bits | (!0u64 << W)) as i64
            } else {
                self.bits as i64
            }
        }
    }
}

impl<const W: u32> Add for Wire<W> {
    type Output = Self;
    fn add(self, rhs: Self) -> Self {
        Self::new(self.bits.wrapping_add(rhs.bits))
    }
}

impl<const W: u32> Sub for Wire<W> {
    type Output = Self;
    fn sub(self, rhs: Self) -> Self {
        Self::new(self.bits.wrapping_sub(rhs.bits))
    }
}

impl<const W: u32> Mul for Wire<W> {
    type Output = Self;
    fn mul(self, rhs: Self) -> Self {
        Self::new(self.bits.wrapping_mul(rhs.bits))
    }
}

impl<const W: u32> BitAnd for Wire<W> {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self {
        Self::new(self.bits & rhs.bits)
    }
}

impl<const W: u32> BitOr for Wire<W> {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self {
        Self::new(self.bits | rhs.bits)
    }
}

impl<const W: u32> BitXor for Wire<W> {
    type Output = Self;
    fn bitxor(self, rhs: Self) -> Self {
        Self::new(self.bits ^ rhs.bits)
    }
}

impl<const W: u32> Not for Wire<W> {
    type Output = Self;
    fn not(self) -> Self {
        Self::new(!self.bits)
    }
}

/// `sel ? a : b`, matching C++ `pyc::cpp::mux`.
pub fn mux<const W: u32>(sel: Wire<1>, a: Wire<W>, b: Wire<W>) -> Wire<W> {
    if sel.to_bool() {
        a
    } else {
        b
    }
}

pub fn eq<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<1> {
    Wire::<1>::new(u64::from(a == b))
}

pub fn ult<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<1> {
    Wire::<1>::new(u64::from(a.value() < b.value()))
}

pub fn slt<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<1> {
    let sign = 1u64 << (W - 1);
    Wire::<1>::new(u64::from((a.value() ^ sign) < (b.value() ^ sign)))
}

pub fn udiv<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<W> {
    if b.value() == 0 {
        Wire::new(0)
    } else {
        Wire::new(a.value() / b.value())
    }
}

pub fn urem<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<W> {
    if b.value() == 0 {
        Wire::new(0)
    } else {
        Wire::new(a.value() % b.value())
    }
}

pub fn sdiv<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<W> {
    if b.value() == 0 {
        return Wire::new(0);
    }
    if a.bit(W - 1) && a.value() == (1u64 << (W - 1)) && b == Wire::ones() {
        return a;
    }
    Wire::new(a.as_signed().wrapping_div(b.as_signed()) as u64)
}

pub fn srem<const W: u32>(a: Wire<W>, b: Wire<W>) -> Wire<W> {
    if b.value() == 0 {
        return Wire::new(0);
    }
    if a.bit(W - 1) && a.value() == (1u64 << (W - 1)) && b == Wire::ones() {
        return Wire::new(0);
    }
    Wire::new(a.as_signed().wrapping_rem(b.as_signed()) as u64)
}

pub fn shl<const W: u32>(v: Wire<W>, amount: u32) -> Wire<W> {
    if amount == 0 {
        v
    } else if amount >= W {
        Wire::new(0)
    } else {
        Wire::new(v.value() << amount)
    }
}

pub fn lshr<const W: u32>(v: Wire<W>, amount: u32) -> Wire<W> {
    if amount == 0 {
        v
    } else if amount >= W {
        Wire::new(0)
    } else {
        Wire::new(v.value() >> amount)
    }
}

pub fn ashr<const W: u32>(v: Wire<W>, amount: u32) -> Wire<W> {
    if amount == 0 {
        return v;
    }
    if amount >= W {
        return if v.bit(W - 1) {
            Wire::ones()
        } else {
            Wire::new(0)
        };
    }
    let mut out = lshr(v, amount);
    if v.bit(W - 1) {
        out = out | shl(Wire::ones(), W - amount);
    }
    out
}

pub fn trunc<const OUT: u32, const IN: u32>(v: Wire<IN>) -> Wire<OUT> {
    Wire::<OUT>::new(v.value())
}

pub fn zext<const OUT: u32, const IN: u32>(v: Wire<IN>) -> Wire<OUT> {
    Wire::<OUT>::new(v.value())
}

pub fn sext<const OUT: u32, const IN: u32>(v: Wire<IN>) -> Wire<OUT> {
    Wire::<OUT>::new(v.as_signed() as u64)
}

pub fn extract<const OUT: u32, const IN: u32>(v: Wire<IN>, lsb: u32) -> Wire<OUT> {
    trunc::<OUT, IN>(lshr(v, lsb))
}

/// MSB-first concat: `a` occupies the high bits, `b` the low bits.
pub fn concat2<const A: u32, const B: u32, const O: u32>(a: Wire<A>, b: Wire<B>) -> Wire<O> {
    Wire::<O>::new((a.value() << B) | b.value())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_masks_to_width() {
        let a = Wire::<4>::new(0xf);
        let b = Wire::<4>::new(0x2);
        assert_eq!((a + b).value(), 0x1);
    }

    #[test]
    fn mux_and_eq() {
        assert_eq!(mux(Wire::<1>::new(1), Wire::<8>::new(3), Wire::<8>::new(9)).value(), 3);
        assert_eq!(eq(Wire::<8>::new(3), Wire::<8>::new(3)).value(), 1);
        assert_eq!(ult(Wire::<8>::new(1), Wire::<8>::new(2)).value(), 1);
        assert_eq!(slt(Wire::<8>::new(0xff), Wire::<8>::new(0x01)).value(), 1);
    }

    #[test]
    fn concat_extract_shift() {
        let hi = Wire::<4>::new(0xa);
        let lo = Wire::<4>::new(0x5);
        let c = concat2::<4, 4, 8>(hi, lo);
        assert_eq!(c.value(), 0xa5);
        assert_eq!(extract::<4, 8>(c, 0).value(), 0x5);
        assert_eq!(extract::<4, 8>(c, 4).value(), 0xa);
        assert_eq!(shl(Wire::<8>::new(1), 3).value(), 8);
        assert_eq!(lshr(Wire::<8>::new(0x80), 4).value(), 8);
        assert_eq!(ashr(Wire::<8>::new(0x80), 4).value(), 0xf8);
    }

    #[test]
    fn div_zero_is_zero() {
        assert_eq!(udiv(Wire::<8>::new(7), Wire::<8>::new(0)).value(), 0);
        assert_eq!(sdiv(Wire::<8>::new(7), Wire::<8>::new(0)).value(), 0);
    }
}
