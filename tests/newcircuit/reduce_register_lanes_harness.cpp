#include "reduce_register_lanes.cpp"

int main() {
  pyc::gen::top sim;
  sim.clk = pyc::cpp::Wire<1>(1);
  sim.rst = pyc::cpp::Wire<1>(1);
  sim.en = pyc::cpp::Wire<1>(1);
  for (unsigned row = 0; row < 3; ++row) {
    sim.init[row][2] = pyc::cpp::Wire<8>(row + 1);
    sim.d[row][2] = pyc::cpp::Wire<8>(row + 7);
  }
  sim.step();
  if (sim.sum2.value() != 6) return 1;
  sim.rst = pyc::cpp::Wire<1>(0);
  sim.clk = pyc::cpp::Wire<1>(0); sim.step();
  sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  if (sim.sum2.value() != 24) return 2;
  sim.d[0][0] = pyc::cpp::Wire<8>(99);
  sim.clk = pyc::cpp::Wire<1>(0); sim.step();
  sim.clk = pyc::cpp::Wire<1>(1); sim.step();
  return sim.sum2.value() == 24 ? 0 : 3;
}
