#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using Dut = pyc::gen::arith;

static void run_functional() {
  Dut dut;
  dut.a = pyc::cpp::Wire<19>(3);
  dut.b = pyc::cpp::Wire<19>(4);
  dut.eval();
  if (dut.sum.value() != 7)
    std::exit(1);
}

static void run_perf(std::uint64_t cycles) {
  Dut dut;
  auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < cycles; ++i) {
    dut.a = pyc::cpp::Wire<19>(static_cast<std::uint64_t>(i));
    dut.b = pyc::cpp::Wire<19>(i * 3);
    dut.eval();
    asm volatile("" : : "r"(dut.sum.value()) : "memory");
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  double hz = secs > 0.0 ? static_cast<double>(cycles) / secs : 0.0;
  std::cout << "{\"backend\":\"cpp\",\"design\":\"arith\",\"cycles\":" << cycles
            << ",\"seconds\":" << secs << ",\"hz\":" << hz << "}\n";
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string(argv[1]) == "perf") {
    std::uint64_t cycles = 1000000;
    if (argc >= 3)
      cycles = std::strtoull(argv[2], nullptr, 10);
    run_perf(cycles);
  } else {
    run_functional();
    std::cout << "ok\n";
  }
  return 0;
}
