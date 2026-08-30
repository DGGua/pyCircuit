//! Two-phase register matching C++ `pyc_reg` (Decision 0001 tick/transfer).
//!
//! State is stored here; wires are passed in on each call so generated modules
//! do not need self-referential structs.

use crate::bits::Wire;

/// Sequential register: sample on posedge in `tick_compute`, publish in `tick_commit`.
#[derive(Clone, Copy, Debug, Default)]
pub struct PycReg<const W: u32> {
    q_next: Wire<W>,
    pending: bool,
    clk_prev: bool,
}

impl<const W: u32> PycReg<W> {
    pub fn new() -> Self {
        Self::default()
    }

    /// Rising-edge sample. Reset wins over enable, matching `pyc_reg::posedge_compute_inner`.
    pub fn tick_compute(
        &mut self,
        clk: Wire<1>,
        rst: Wire<1>,
        en: Wire<1>,
        d: Wire<W>,
        init: Wire<W>,
    ) {
        let clk_now = clk.to_bool();
        let posedge = !self.clk_prev && clk_now;
        self.clk_prev = clk_now;
        if !posedge {
            self.pending = false;
            return;
        }
        let r = rst.to_bool();
        self.pending = r || en.to_bool();
        self.q_next = if r { init } else { d };
    }

    pub fn tick_commit(&mut self, q: &mut Wire<W>) {
        if self.pending {
            *q = self.q_next;
            self.pending = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bits::Wire;

    #[test]
    fn posedge_enable_then_commit() {
        let mut r = PycReg::<8>::new();
        let mut q = Wire::<8>::new(0);
        r.tick_compute(
            Wire::<1>::new(1),
            Wire::<1>::new(0),
            Wire::<1>::new(1),
            Wire::<8>::new(7),
            Wire::<8>::new(0),
        );
        assert_eq!(q.value(), 0);
        r.tick_commit(&mut q);
        assert_eq!(q.value(), 7);
    }

    #[test]
    fn reset_beats_enable() {
        let mut r = PycReg::<8>::new();
        let mut q = Wire::<8>::new(9);
        r.tick_compute(
            Wire::<1>::new(1),
            Wire::<1>::new(1),
            Wire::<1>::new(1),
            Wire::<8>::new(7),
            Wire::<8>::new(3),
        );
        r.tick_commit(&mut q);
        assert_eq!(q.value(), 3);
    }

    #[test]
    fn no_update_without_posedge() {
        let mut r = PycReg::<8>::new();
        let mut q = Wire::<8>::new(1);
        r.tick_compute(
            Wire::<1>::new(1),
            Wire::<1>::new(0),
            Wire::<1>::new(1),
            Wire::<8>::new(4),
            Wire::<8>::new(0),
        );
        r.tick_commit(&mut q);
        r.tick_compute(
            Wire::<1>::new(1),
            Wire::<1>::new(0),
            Wire::<1>::new(1),
            Wire::<8>::new(5),
            Wire::<8>::new(0),
        );
        r.tick_commit(&mut q);
        assert_eq!(q.value(), 4);
    }
}
