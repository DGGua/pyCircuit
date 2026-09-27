#include "primitive_activity_checks.h"
#include <deque>

int main() {
  pyc::gen::top sim;
  std::deque<unsigned> queue;
  unsigned consumedCount = 0;
  auto check = [&]() {
    sim.eval();
    bool valid = sim.out_valid.toBool();
    unsigned data = sim.out_data.value();
    if (valid)
      require(!queue.empty() && data == queue.front(), "async FIFO queue oracle mismatch");
    else
      require(data == 0, "async FIFO empty output is not zero");
    require(sim.y.value() == (((data + data) & 255u) ^ data), "async FIFO stale data consumer");
    require(sim.status.toBool() == (sim.in_ready.toBool() != valid),
            "async FIFO stale status consumer");
  };
  for (unsigned event = 0; event < 1056; ++event) {
    sim.in_clk = pyc::cpp::Wire<1>(0);
    sim.out_clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    bool drain = event >= 1024;
    bool reset = !drain && event % 97 == 0;
    bool inEdge = reset || drain || event % 3 != 0;
    bool outEdge = reset || drain || event % 5 != 0;
    bool push = !drain && event % 11 < 8;
    bool pop = drain || event % 13 > 3;
    unsigned data = (event * 73u + 5) & 255u;
    sim.in_rst = pyc::cpp::Wire<1>(reset);
    sim.out_rst = pyc::cpp::Wire<1>(reset);
    sim.in_valid = pyc::cpp::Wire<1>(push);
    sim.in_data = pyc::cpp::Wire<8>(data);
    sim.out_ready = pyc::cpp::Wire<1>(pop);
    check();
    bool accepted = inEdge && push && sim.in_ready.toBool();
    bool consumed = outEdge && pop && sim.out_valid.toBool();
    sim.in_clk = pyc::cpp::Wire<1>(inEdge);
    sim.out_clk = pyc::cpp::Wire<1>(outEdge);
    sim.tick_compute();
    check();
    sim.tick_commit();
    if (reset) queue.clear();
    else {
      if (consumed) {
        queue.pop_front();
        ++consumedCount;
      }
      if (accepted) queue.push_back(data);
      require(queue.size() <= 4, "async FIFO accepted past capacity");
    }
    checkStable(sim, check);
    sim.tick_commit();
    check();
  }
  require(queue.empty() && consumedCount > 100, "async FIFO failed to drain");
  std::cout << "async FIFO activity oracle passed (1056 clock events)\n";
}
