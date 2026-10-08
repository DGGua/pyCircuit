#pragma once
#include "activity.cpp"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

inline void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}

inline unsigned long long groupCalls(pyc::gen::top &sim) {
  std::ostringstream stats;
  sim.dump_sim_stats(stats);
  const std::string key = "group_eval_calls=";
  const std::string text = stats.str();
  auto position = text.find(key);
  require(position != std::string::npos, "missing group activity counter");
  return std::stoull(text.substr(position + key.size()));
}

template <typename Check>
void checkStable(pyc::gen::top &sim, Check check) {
  check();
  const auto calls = groupCalls(sim);
  check();
  check();
  if (std::getenv("EXPECT_ACTIVITY"))
    require(calls != 0 && calls == groupCalls(sim), "stable primitive output reactivated a group");
}
