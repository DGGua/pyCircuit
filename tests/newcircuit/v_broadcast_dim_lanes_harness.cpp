#include "v_broadcast_dim_lanes.cpp"
int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 32; ++step) {
    sim.a[0] = pyc::cpp::Wire<8>(step);
    sim.a[1] = pyc::cpp::Wire<8>(255 - step);
    sim.eval();
    if (sim.row_item.value() != 255 - step) return 1;
    for (unsigned lane = 0; lane < 3; ++lane)
      if (sim.col_row[lane].value() != 255 - step) return 2;
  }
  return 0;
}
