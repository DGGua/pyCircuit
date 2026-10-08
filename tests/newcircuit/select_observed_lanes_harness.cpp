#include "select_observed_lanes.cpp"

int main() {
  pyc::gen::top sim;
  for (unsigned step = 0; step < 256; ++step) {
    sim.sel = pyc::cpp::Wire<1>(step & 1u);
    for (unsigned lane = 0; lane < 4; ++lane) {
      sim.a[lane] = pyc::cpp::Wire<8>((step + lane * 17u) & 255u);
      sim.b[lane] = pyc::cpp::Wire<8>((step * 3u + lane * 29u) & 255u);
    }
    sim.eval();
    unsigned expected0 = (step & 1u) ? step : step * 3u;
    unsigned expected3 = (step & 1u) ? step + 51u : step * 3u + 87u;
    if (sim.lane0.value() != (expected0 & 255u) ||
        sim.lane3.value() != (expected3 & 255u))
      return 1;
  }
  return 0;
}
