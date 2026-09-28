#ifdef PYC_SPLIT_MODEL
#include "top.hpp"
#else
#include "large_state_probe.cpp"
#endif
#include "large_state_probe_count.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
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
  constexpr unsigned N = LARGE_STATE_COUNT;
  auto model = std::make_unique<pyc::gen::top>();
  auto &sim = *model;
  pyc::cpp::ProbeRegistry registry;
  sim.pyc_register_probes(registry, "dut");
  std::array<const pyc::cpp::ProbeRegistry::Entry *, N> probes{};
  std::array<std::uint16_t, N> expected{}, next{};
  for (unsigned i = 0; i < N; ++i) {
    char name[32]; std::snprintf(name, sizeof(name), "dut:state_%04u", i);
    probes[i] = registry.findByPath(name);
    const auto *p = probes[i];
    if (!p || p->kind != pyc::cpp::ProbeKind::Reg || p->width_bits != 16 ||
        !p->write_valid || !p->write_data_ptr) return 1;
  }
  const auto registered = registry.findByKind(pyc::cpp::ProbeKind::Reg);
  if (registered.size() != N + 1 || registered[0]->path != "dut:y") return 11;
  for (unsigned i = 0; i < N; ++i)
    if (registered[i + 1] != probes[i]) return 12;
  auto check = [&](unsigned cycle) {
    sim.eval();
    for (unsigned i = 0; i < N; ++i) {
      auto value = static_cast<const pyc::cpp::Wire<16> *>(probes[i]->ptr)->value();
      if (value != expected[i]) {
        std::fprintf(stderr, "large state mismatch cycle=%u lane=%u got=%u expected=%u\n",
                     cycle, i, unsigned(value), unsigned(expected[i]));
        return false;
      }
    }
    return sim.y.value() == expected[0];
  };
  std::uint64_t digest = 1469598103934665603ull;
  for (unsigned cycle = 0; cycle < 96; ++cycle) {
    sim.clk = pyc::cpp::Wire<1>(0); sim.tick_compute(); sim.tick_commit();
    const bool reset[] = {cycle % 17 == 0, cycle % 23 == 0};
    const bool enable[] = {cycle % 3 != 0, cycle % 5 != 0, cycle % 7 != 0, true};
    const std::uint16_t data = cycle * 977 + 31;
    sim.rst0 = pyc::cpp::Wire<1>(reset[0]); sim.rst1 = pyc::cpp::Wire<1>(reset[1]);
    sim.en0 = pyc::cpp::Wire<1>(enable[0]); sim.en1 = pyc::cpp::Wire<1>(enable[1]);
    sim.en2 = pyc::cpp::Wire<1>(enable[2]); sim.data = pyc::cpp::Wire<16>(data);
    if (!check(cycle)) return 2;
    for (unsigned i = 0; i < N; ++i)
      next[i] = reset[i >= N / 2] ? std::uint16_t(i * 13 + 7) :
          enable[i % 4] ? std::uint16_t(expected[i] + data + i * 37 + 11) : expected[i];
    sim.clk = pyc::cpp::Wire<1>(1); sim.tick_compute();
    if (!check(cycle)) return 3; // TICK exposes old state; pending writes are separate.
    for (unsigned i = 0; i < N; ++i) {
      bool writes = reset[i >= N / 2] || enable[i % 4];
      if (*probes[i]->write_valid != writes || (writes &&
          static_cast<const pyc::cpp::Wire<16> *>(probes[i]->write_data_ptr)->value() != next[i])) return 4;
    }
    sim.tick_commit(); expected = next;
    if (!check(cycle)) return 5;
    for (auto value : expected) digest = (digest ^ value) * 1099511628211ull;
    sim.tick_commit();
    if (!check(cycle)) return 6;
    sim.data = pyc::cpp::Wire<16>(data ^ 65535u);
    sim.rst0 = pyc::cpp::Wire<1>(!reset[0]); sim.rst1 = pyc::cpp::Wire<1>(!reset[1]);
    sim.eval(); sim.tick_compute(); sim.tick_commit();
    if (!check(cycle)) return 7;
    sim.tick_compute(); sim.tick_commit();
    if (!check(cycle)) return 8;
    auto stable = counter(sim, "group_eval_calls");
    for (unsigned repeat = 0; repeat < 3; ++repeat) if (!check(cycle)) return 9;
    if (counter(sim, "group_eval_calls") != stable) return 10;
  }
  // API-specific phase test: a second compute without a new clock edge
  // cancels pending writes, even if the first compute has not been committed.
  sim.clk = pyc::cpp::Wire<1>(0); sim.tick_compute(); sim.tick_commit();
  sim.rst0 = sim.rst1 = sim.en0 = sim.en1 = sim.en2 = pyc::cpp::Wire<1>(1);
  sim.eval(); sim.clk = pyc::cpp::Wire<1>(1); sim.tick_compute();
  for (auto *p : probes) if (!*p->write_valid) return 13;
  sim.tick_compute();
  for (auto *p : probes) if (*p->write_valid) return 14;
  sim.tick_commit();
  if (!check(96)) return 15;
  std::printf("Large state/probe oracle PASS: %u checks; digest=%016llx\n", N * 96,
              static_cast<unsigned long long>(digest));
}
