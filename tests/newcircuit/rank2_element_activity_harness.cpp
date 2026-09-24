#include "rank2_element_activity.cpp"
#include <sstream>
#include <string>

static unsigned evaluations(pyc::gen::top &sim) {
  std::ostringstream out;
  sim.dump_sim_stats(out);
  const std::string key = "group_eval_calls=";
  const std::string text = out.str();
  auto pos = text.find(key);
  return pos == std::string::npos ? 0u
                                  : static_cast<unsigned>(std::stoul(text.substr(pos + key.size())));
}

int main() {
  pyc::gen::top sim;
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned col = 0; col < 4; ++col)
      sim.v[row][col] = pyc::cpp::Wire<8>(row + col);
  sim.eval();
  if (sim.y.value() != 10) return 1;
  unsigned initial = evaluations(sim);
  if (!initial) return 2;
  sim.v[1][0] = pyc::cpp::Wire<8>(99);
  sim.eval();
  if (sim.y.value() != 10 || evaluations(sim) != initial) return 3;
  sim.v[1][2] = pyc::cpp::Wire<8>(13);
  sim.eval();
  if (sim.y.value() != 20 || evaluations(sim) <= initial) return 4;
  return 0;
}
