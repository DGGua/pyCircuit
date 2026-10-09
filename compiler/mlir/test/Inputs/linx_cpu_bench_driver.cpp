#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include <cpp/pyc_tb.hpp>

#include "linx_cpu_pyc.hpp"

// Long-run benchmark driver: fixed cycle count, prints steady-state
// throughput and a state digest for cross-build equivalence checks.
using pyc::cpp::Testbench;
using pyc::cpp::Wire;

int main(int argc, char **argv) {
  std::uint64_t target = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 200000;

  pyc::gen::linx_cpu_pyc dut{};
  dut.boot_pc = Wire<64>(0x10000ull);
  dut.boot_sp = Wire<64>(0x0fff00ull);
  dut.irq = Wire<1>(0);
  dut.irq_vector = Wire<64>(0);
  dut.host_wvalid = Wire<1>(0);
  dut.host_waddr = Wire<64>(0);
  dut.host_wdata = Wire<64>(0);
  dut.host_wstrb = Wire<8>(0);

  Testbench<pyc::gen::linx_cpu_pyc> tb(dut);
  tb.addClock(dut.clk, 1);
  tb.reset(dut.rst, 2, 1);

  auto t0 = std::chrono::steady_clock::now();
  std::uint64_t n = 0;
  while (n < target) {
    if (dut.halted.toBool()) {
      // Re-run the boot program from a clean state to keep the pipeline busy.
      tb.reset(dut.rst, 2, 1);
      continue;
    }
    tb.runCyclesAuto(1);
    ++n;
  }
  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();

  // State digest: all observable outputs after the run.
  std::uint64_t h = 1469598103934665603ull;
  auto mix = [&](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  mix(dut.pc.value());
  mix(dut.cycles.value());
  mix(dut.halted.value());
  mix(dut.exit_code.value());
  mix(dut.uart_valid.value());
  mix(dut.uart_byte.value());
  mix(dut.a0.value());
  mix(dut.a1.value());
  mix(dut.ra.value());
  mix(dut.sp.value());

  std::cout << "bench cycles=" << n << " seconds=" << sec
            << " cycles_per_sec=" << (sec > 0 ? static_cast<double>(n) / sec : 0.0)
            << " digest=" << std::hex << h << std::dec << "\n";
  return 0;
}
