#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using Dut = pyc::gen::counter;

static void full_cycle(Dut &dut) {
  dut.clk = pyc::cpp::Wire<1>(1);
  dut.eval();
  dut.tick();
  dut.transfer();
  dut.eval();
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.eval();
  dut.tick();
  dut.transfer();
  dut.eval();
}

static void run_functional() {
  Dut dut;
  dut.enable = pyc::cpp::Wire<1>(0);
  dut.rst = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.eval();
  for (int i = 0; i < 2; ++i)
    full_cycle(dut);
  dut.rst = pyc::cpp::Wire<1>(0);
  full_cycle(dut);
  dut.enable = pyc::cpp::Wire<1>(1);
  for (std::uint64_t expect = 1; expect <= 5; ++expect) {
    full_cycle(dut);
    std::uint64_t got = dut.count.value();
    std::cout << "count=" << got << "\n";
    if (got != expect)
      std::exit(1);
  }
}

static void run_perf(std::uint64_t cycles) {
  Dut dut;
  dut.enable = pyc::cpp::Wire<1>(1);
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.eval();
  auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < cycles; ++i)
    full_cycle(dut);
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  double hz = secs > 0.0 ? static_cast<double>(cycles) / secs : 0.0;
  std::cout << "{\"backend\":\"cpp\",\"design\":\"counter\",\"cycles\":" << cycles
            << ",\"seconds\":" << secs << ",\"hz\":" << hz << "}\n";
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string(argv[1]) == "perf") {
    std::uint64_t cycles = 200000;
    if (argc >= 3)
      cycles = std::strtoull(argv[2], nullptr, 10);
    run_perf(cycles);
  } else {
    run_functional();
    std::cout << "ok\n";
  }
  return 0;
}
