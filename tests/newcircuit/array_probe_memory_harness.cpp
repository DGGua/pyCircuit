#include "array_probe_memory.cpp"
#include <array>
#include <iomanip>
#include <sstream>

int main() {
  pyc::gen::top sim;
  pyc::cpp::ProbeRegistry probes;
  sim.pyc_register_probes(probes, "dut");
  const auto *entry16 = probes.findByPath("dut:mem16");
  const auto *entry32 = probes.findByPath("dut:mem32");
  if (!entry16 || !entry32 || entry16->kind != pyc::cpp::ProbeKind::Mem ||
      entry32->kind != pyc::cpp::ProbeKind::Mem) return 1;
  auto *memory16 = static_cast<pyc::cpp::pyc_sync_mem<2, 16, 4> *>(entry16->ptr);
  auto *memory32 = static_cast<pyc::cpp::pyc_sync_mem_dp<2, 32, 4> *>(entry32->ptr);
  memory16->mem_watch(0, 3); memory32->mem_watch(0, 3);
  std::array<uint32_t, 4> m16{}, m32{};
  auto checkStorage = [&](auto *memory, const auto &model, unsigned digits) {
    uint64_t hash = 1469598103934665603ull;
    std::ostringstream expected, actual;
    for (unsigned address = 0; address < 4; ++address) {
      hash = (hash ^ model[address]) * 1099511628211ull;
      expected << "{\"addr\":" << address << ",\"data\":\"0x" << std::hex
               << std::setw(digits) << std::setfill('0') << model[address]
               << std::dec << "\"}\n";
      if (memory->peekEntry(address) != model[address]) return false;
    }
    memory->mem_dump(actual);
    return memory->mem_hash() == hash && actual.str() == expected.str();
  };
  unsigned read16 = 0, read32a = 0, read32b = 0;
  for (unsigned step = 0; step < 256; ++step) {
    bool reset = step % 19 == 0, en0 = step % 3 != 0, en1 = step % 5 != 0;
    bool write = step % 7 != 0;
    unsigned addr0 = step % 4, addr1 = (step + 1) % 4;
    uint32_t data = (((step * 0x01010101u) ^ 0x89abcdefu) & 0xffffff00u) | 0x5au;
    unsigned mask = step < 8 ? 15 : (1u << (step % 4));
    sim.rst = pyc::cpp::Wire<1>(reset); sim.ren0 = pyc::cpp::Wire<1>(en0);
    sim.ren1 = pyc::cpp::Wire<1>(en1); sim.wvalid = pyc::cpp::Wire<1>(write);
    sim.raddr0 = pyc::cpp::Wire<2>(addr0); sim.raddr1 = pyc::cpp::Wire<2>(addr1);
    sim.waddr = pyc::cpp::Wire<2>(addr0); sim.wdata = pyc::cpp::Wire<32>(data);
    sim.wstrb = pyc::cpp::Wire<4>(mask);
    sim.clk = pyc::cpp::Wire<1>(0); sim.step();
    memory16->mem_watch_clear(); memory32->mem_watch_clear();
    sim.clk = pyc::cpp::Wire<1>(1); sim.comb(); sim.tick();
    if (!checkStorage(memory16, m16, 4) || !checkStorage(memory32, m32, 8) ||
        sim.low16.value() != read16 || sim.low32a.value() != read32a ||
        sim.low32b.value() != read32b) return 2;
    if (reset) read16 = read32a = read32b = 0;
    else {
      if (en0) { read16 = m16[addr0] & 255; read32a = m32[addr0] & 255; }
      if (en1) read32b = m32[addr1] & 255;
      if (write)
        for (unsigned byte = 0; byte < 4; ++byte)
          if (mask & (1u << byte)) {
            uint32_t bits = 255u << (byte * 8);
            m32[addr0] = (m32[addr0] & ~bits) | (data & bits);
            if (byte < 2) m16[addr0] = (m16[addr0] & ~bits) | (data & bits);
          }
    }
    sim.transfer(); sim.comb();
    if (!checkStorage(memory16, m16, 4) || !checkStorage(memory32, m32, 8) ||
        sim.low16.value() != read16 || sim.low32a.value() != read32a ||
        sim.low32b.value() != read32b) return 3;
    for (const auto &event : memory16->mem_watch_events())
      if (static_cast<unsigned>(event.kind) == 1 && event.data.value() != m16[event.addr]) return 4;
    for (const auto &event : memory32->mem_watch_events())
      if (static_cast<unsigned>(event.kind) == 1 && event.data.value() != m32[event.addr]) return 5;
  }
}
