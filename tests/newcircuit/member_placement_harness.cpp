#ifdef PYC_SPLIT_MODEL
#include "top.hpp"
#else
#include "member_placement.cpp"
#endif
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

static unsigned long long counter(pyc::gen::top &sim, const char *name) {
  std::ostringstream out;
  sim.dump_sim_stats(out);
  const auto text = out.str(), key = std::string(name) + "=";
  const auto pos = text.find(key);
  if (pos == std::string::npos) std::abort();
  return std::stoull(text.substr(pos + key.size()));
}
int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry registry;
  sim.pyc_register_probes(registry, "dut");
  auto *named = registry.findByPath("dut:debug_named");
  auto *nested = registry.findByPath("dut:debug_nested");
  auto *wide = registry.findByPath("dut:debug_wide");
  if (!named || !nested || !wide || named->width_bits != 8 || nested->width_bits != 8 ||
      wide->width_bits != 128 || named->kind != pyc::cpp::ProbeKind::Wire ||
      nested->kind != pyc::cpp::ProbeKind::Wire || wide->kind != pyc::cpp::ProbeKind::Wire) return 1;
  std::uint64_t signature = 1469598103934665603ull;
  for (const auto *entry : registry.findByGlob("dut:*")) {
    signature = (signature ^ entry->probe_id) * 1099511628211ull;
    signature = (signature ^ entry->width_bits) * 1099511628211ull;
    signature = (signature ^ unsigned(entry->kind)) * 1099511628211ull;
  }
  std::uint64_t digest = 1469598103934665603ull;
  std::uint32_t random = 0x15a43f9du;
  auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
  for (unsigned iteration = 0; iteration < 2048; ++iteration) {
    const std::uint8_t a = next(), b = next(), c = next();
    const bool sel = (iteration & 3u) < 2u;
    const std::uint64_t x = iteration < 32 ? ~std::uint64_t(iteration) : (std::uint64_t(next()) << 32) | next();
    const std::uint64_t z = iteration < 32 ? ~std::uint64_t(iteration * 7u) : (std::uint64_t(next()) << 32) | next();
    sim.a = pyc::cpp::Wire<8>(a); sim.b = pyc::cpp::Wire<8>(b); sim.c = pyc::cpp::Wire<8>(c);
    sim.sel = pyc::cpp::Wire<1>(sel); sim.x = pyc::cpp::Wire<64>(x); sim.z = pyc::cpp::Wire<64>(z);
    const std::uint8_t sum = a + b, mix = sum ^ c;
    const std::uint8_t choice = sel ? mix : b, incremented = choice + a;
    const std::uint8_t out = std::uint8_t(incremented ^ sum) * b;
    const std::uint8_t difference = a - b;
    const std::uint8_t nested_out = std::uint8_t(difference ^ b) * c + a;
    // An independent two-word unsigned arithmetic oracle; do not call Wire math.
    const std::uint64_t double_lo = z + z, double_hi = x + x + (double_lo < z);
    const std::uint64_t masked_lo = double_lo ^ UINT64_C(0x0fedcba987654321);
    const std::uint64_t masked_hi = double_hi ^ UINT64_C(0x123456789abcdef0);
    const std::uint64_t result_lo = masked_lo + z, result_hi = masked_hi + x + (result_lo < masked_lo);
    auto check = [&]() {
      sim.eval();
      const auto &wp = *static_cast<const pyc::cpp::Wire<128> *>(wide->ptr);
      return sim.out.value() == out && sim.nested_out.value() == nested_out &&
          sim.wide_out.word(0) == result_lo && sim.wide_out.word(1) == result_hi &&
          static_cast<const pyc::cpp::Wire<8> *>(named->ptr)->value() == mix &&
          static_cast<const pyc::cpp::Wire<8> *>(nested->ptr)->value() == difference &&
          wp.word(0) == double_lo && wp.word(1) == double_hi;
    };
    if (!check()) { std::fprintf(stderr, "member placement oracle mismatch case=%u\n", iteration); return 2; }
    const auto evaluated = counter(sim, "group_eval_calls");
    for (unsigned repeat = 0; repeat < 3; ++repeat) if (!check()) return 3;
    if (counter(sim, "group_eval_calls") != evaluated) return 4;
    for (auto value : {std::uint64_t(out), std::uint64_t(nested_out), result_lo, result_hi,
                       std::uint64_t(mix), std::uint64_t(difference), double_lo, double_hi})
      digest = (digest ^ value) * 1099511628211ull;
  }
  if (!counter(sim, "group_eval_calls") || !counter(sim, "group_cache_skips")) return 5;
  std::printf("Member placement oracle PASS: 2048 cases; digest=%016llx; probes=%016llx\n",
              static_cast<unsigned long long>(digest), static_cast<unsigned long long>(signature));
}
