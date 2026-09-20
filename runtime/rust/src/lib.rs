//! pyCircuit Rust simulation runtime (experimental).
//!
//! Generated modules use native `bool`/`u8`/`u16`/`u32`/`u64` fields. This
//! crate supplies two-phase `PycReg<T>` and helpers for signed/wide ops.

pub mod bits;
pub mod reg;

pub use bits::{ashr_bits, sdiv_bits, sext_bits, slt_bits, srem_bits, udiv_bits, urem_bits};
pub use reg::PycReg;
