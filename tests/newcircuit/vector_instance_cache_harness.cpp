#ifdef PYC_SPLIT_MODEL
#include "top.hpp"
#else
#include "vector_instance_cache.cpp"
#endif
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

static unsigned long long counter(pyc::gen::top &sim, const char *name) {
  std::ostringstream out;
  sim.dump_sim_stats(out);
  const std::string text = out.str(), key = std::string(name) + "=";
  const auto pos = text.find(key);
  if (pos == std::string::npos) std::abort();
  return std::stoull(text.substr(pos + key.size()));
}

int main() {
  pyc::gen::top sim;
  unsigned small[6]{};
  std::uint64_t wide[6][16]{};
  auto check = [&](unsigned changed) {
    const auto evaluated = counter(sim, "instance_eval_calls");
    const auto skipped = counter(sim, "instance_cache_skips");
    sim.eval();
    for (unsigned lane = 0; lane < 6; ++lane) {
      if (sim.small_out[lane / 3][lane % 3].value() != ((small[lane] * 2u) & 255u))
        return false;
      for (unsigned word = 0; word < 16; ++word)
        if (sim.wide_out[lane / 3][lane % 3].word(word) != ~wide[lane][word])
          return false;
    }
#ifdef PYC_DISABLE_INSTANCE_EVAL_CACHE
    changed = 2;
#endif
    return counter(sim, "instance_eval_calls") - evaluated == changed &&
           counter(sim, "instance_cache_skips") - skipped == 2u - changed;
  };
  if (!check(2) || !check(0)) return 1;
  for (unsigned step = 0; step < 2048; ++step) {
    const unsigned lane = (step / 2) % 6;
    if ((step & 1) == 0) {
      small[lane] = (small[lane] + 1u) & 255u;
      sim.small[lane / 3][lane % 3] = pyc::cpp::Wire<8>(small[lane]);
    } else {
      const unsigned word = (step / 12) % 16;
      wide[lane][word] ^= UINT64_C(0x9e3779b97f4a7c15) + step;
      sim.wide[lane / 3][lane % 3].setWord(word, wide[lane][word]);
    }
    if (!check(1) || !check(0)) {
      std::cerr << "vector instance cache mismatch at " << step << '\n';
      return 2;
    }
  }
  std::cout << "vector instance cache oracle passed (2048 lane/word changes)\n";
}
