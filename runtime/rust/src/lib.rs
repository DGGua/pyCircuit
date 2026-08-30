//! pyCircuit Rust simulation runtime (experimental v1 subset).
//!
//! Only scalar `Wire<1..=64>` and two-phase `PycReg` are provided. Generated
//! modules own child instances via `Box<T>` (Decision 0012).

pub mod bits;
pub mod reg;

pub use bits::{
    ashr, concat2, eq, extract, lshr, mux, sdiv, sext, shl, slt, srem, trunc, udiv, ult, urem, zext,
    Wire,
};
pub use reg::PycReg;
