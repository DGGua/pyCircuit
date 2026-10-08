#include "primitive_activity_checks.h"
#include <deque>

int main() {
  pyc::gen::top sim;
  std::deque<unsigned> queue;
  auto check = [&]() {
    sim.eval();
    bool valid = !queue.empty();
    unsigned data = valid ? queue.front() : 0;
    bool ready = queue.size() < 4 || (valid && sim.out_ready.toBool());
    require(sim.in_ready.toBool() == ready && sim.out_valid.toBool() == valid,
            "FIFO ready/valid oracle mismatch");
    require(sim.out_data.value() == data, "FIFO queue oracle mismatch");
    require(sim.y.value() == (((data + data) & 255u) ^ data), "FIFO stale data consumer");
    require(sim.status.toBool() == (ready != valid), "FIFO stale status consumer");
  };
  for (unsigned cycle = 0; cycle < 1024; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0);
    sim.tick_compute();
    sim.tick_commit();
    bool reset = cycle % 73 == 0;
    bool push = (cycle % 23) < 17;
    bool pop = (cycle % 19) > 6;
    unsigned data = (cycle * 91u) & 255u;
    sim.rst = pyc::cpp::Wire<1>(reset);
    sim.in_valid = pyc::cpp::Wire<1>(push);
    sim.in_data = pyc::cpp::Wire<8>(data);
    sim.out_ready = pyc::cpp::Wire<1>(pop);
    check();
    bool accepted = push && sim.in_ready.toBool();
    bool consumed = pop && sim.out_valid.toBool();
    sim.clk = pyc::cpp::Wire<1>(1);
    sim.tick_compute();
    check(); // Current-state outputs are unchanged before commit.
    sim.tick_commit();
    if (reset) queue.clear();
    else {
      if (consumed) queue.pop_front();
      if (accepted) queue.push_back(data);
    }
    checkStable(sim, check);
    sim.tick_commit();
    check();
  }
  std::cout << "FIFO activity oracle passed (1024 cycles)\n";
}
