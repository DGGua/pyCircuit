//! Two-phase register. Control pins are `bool`; data is the generated wire type.

use core::marker::PhantomData;

/// Sequential register: sample on posedge in `tick_compute`, publish in `tick_commit`.
#[derive(Clone, Copy, Debug)]
pub struct PycReg<T: Copy + Default> {
    q_next: T,
    pending: bool,
    clk_prev: bool,
    _ty: PhantomData<T>,
}

impl<T: Copy + Default> Default for PycReg<T> {
    fn default() -> Self {
        Self {
            q_next: T::default(),
            pending: false,
            clk_prev: false,
            _ty: PhantomData,
        }
    }
}

impl<T: Copy + Default> PycReg<T> {
    pub fn new() -> Self {
        Self::default()
    }

    /// Rising-edge sample. Reset wins over enable, matching C++ `pyc_reg`.
    pub fn tick_compute(&mut self, clk: bool, rst: bool, en: bool, d: T, init: T) {
        let posedge = !self.clk_prev && clk;
        self.clk_prev = clk;
        if !posedge {
            self.pending = false;
            return;
        }
        self.pending = rst || en;
        self.q_next = if rst { init } else { d };
    }

    pub fn tick_commit(&mut self, q: &mut T) {
        if self.pending {
            *q = self.q_next;
            self.pending = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn posedge_enable_then_commit() {
        let mut r = PycReg::<u8>::new();
        let mut q = 0u8;
        r.tick_compute(true, false, true, 7, 0);
        assert_eq!(q, 0);
        r.tick_commit(&mut q);
        assert_eq!(q, 7);
    }

    #[test]
    fn reset_beats_enable() {
        let mut r = PycReg::<u8>::new();
        let mut q = 9u8;
        r.tick_compute(true, true, true, 7, 3);
        r.tick_commit(&mut q);
        assert_eq!(q, 3);
    }
}
