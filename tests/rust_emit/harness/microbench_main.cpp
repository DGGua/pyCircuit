#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using Dut = pyc::gen::microbench;

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
  dut.rst = pyc::cpp::Wire<1>(1);
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.sel = pyc::cpp::Wire<1>(1);
  dut.addend = pyc::cpp::Wire<16>(1);
  dut.eval();
  for (int i = 0; i < 2; ++i)
    full_cycle(dut);
  dut.rst = pyc::cpp::Wire<1>(0);
  for (int i = 0; i < 8; ++i)
    full_cycle(dut);
  std::cout << "acc_out=" << dut.acc_out.value() << "\n";
}

static void run_perf(std::uint64_t cycles) {
  Dut dut;
  dut.rst = pyc::cpp::Wire<1>(0);
  dut.sel = pyc::cpp::Wire<1>(1);
  dut.addend = pyc::cpp::Wire<16>(1);
  dut.clk = pyc::cpp::Wire<1>(0);
  dut.eval();
  auto t0 = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < cycles; ++i)
    full_cycle(dut);
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  double hz = secs > 0.0 ? static_cast<double>(cycles) / secs : 0.0;
  std::cout << "{\"backend\":\"cpp\",\"design\":\"microbench\",\"cycles\":" << cycles
            << ",\"seconds\":" << secs << ",\"hz\":" << hz << "}\n";
}

int main(int argc, char **argv) {
  if (argc >= 2 && std::string(argv[1]) == "perf") {
    std::uint64_t cycles = 50000;
    if (argc >= 3)
      cycles = std::strtoull(argv[2], nullptr, 10);
    run_perf(cycles);
  } else {
    run_functional();
    std::cout << "ok\n";
  }
  return 0;
}
