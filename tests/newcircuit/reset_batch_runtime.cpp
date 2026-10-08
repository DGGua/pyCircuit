#include "cpp/pyc_primitives.hpp"
#include "cpp/pyc_vec.hpp"

#include <cstdint>

int main() {
  using pyc::cpp::Wire;
  Wire<1> clk(0), rst(0), en0(0), en1(0);
  Wire<8> d0(0), d1(0), init0(0x12), init1(0x34);
  Wire<8> referenceQ0(0), referenceQ1(0), batchedQ0(0), batchedQ1(0);
  pyc::cpp::pyc_reg<8> reference0(clk, rst, en0, d0, init0, referenceQ0);
  pyc::cpp::pyc_reg<8> reference1(clk, rst, en1, d1, init1, referenceQ1);
  pyc::cpp::pyc_reg<8> batched0(clk, rst, en0, d0, init0, batchedQ0);
  pyc::cpp::pyc_reg<8> batched1(clk, rst, en1, d1, init1, batchedQ1);
  bool previousClock = false;
  std::uint32_t random = 0x973abce1u;
  auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    std::uint32_t controls = next();
    clk = Wire<1>(controls & 1u);
    rst = Wire<1>((controls >> 1) & 1u);
    en0 = Wire<1>((controls >> 2) & 1u);
    en1 = Wire<1>((controls >> 3) & 1u);
    d0 = Wire<8>(next() & 255u);
    d1 = Wire<8>(next() & 255u);
    reference0.tick_compute();
    reference1.tick_compute();
    bool clockNow = clk.toBool();
    bool edge = !previousClock && clockNow;
    previousClock = clockNow;
    if (edge) {
      if (rst.toBool()) {
        batched0.posedge_reset_compute();
        batched1.posedge_reset_compute();
      } else {
        batched0.posedge_data_compute();
        batched1.posedge_data_compute();
      }
    } else if (clockNow) {
      batched0.noedge_update();
      batched1.noedge_update();
    } else {
      batched0.negedge_update();
      batched1.negedge_update();
    }
    if (reference0.pending != batched0.pending ||
        reference1.pending != batched1.pending ||
        reference0.clkPrev != batched0.clkPrev ||
        reference1.clkPrev != batched1.clkPrev ||
        reference0.qNext != batched0.qNext ||
        reference1.qNext != batched1.qNext)
      return 1;
    if (next() & 1u) {
      reference0.tick_commit();
      reference1.tick_commit();
      batched0.tick_commit();
      batched1.tick_commit();
      if (referenceQ0 != batchedQ0 || referenceQ1 != batchedQ1)
        return 2;
    }
  }
  using Vector = pyc::cpp::Vec<Wire<8>, 2>;
  Vector vectorData0{}, vectorData1{}, vectorInit0{}, vectorInit1{};
  Vector vectorReferenceQ0{}, vectorReferenceQ1{};
  Vector vectorBatchedQ0{}, vectorBatchedQ1{};
  vectorInit0[0] = Wire<8>(0x12);
  vectorInit0[1] = Wire<8>(0x56);
  vectorInit1[0] = Wire<8>(0x34);
  vectorInit1[1] = Wire<8>(0x78);
  pyc::cpp::pyc_vec_reg<Vector> vectorReference0(
      clk, rst, en0, vectorData0, vectorInit0, vectorReferenceQ0);
  pyc::cpp::pyc_vec_reg<Vector> vectorReference1(
      clk, rst, en1, vectorData1, vectorInit1, vectorReferenceQ1);
  pyc::cpp::pyc_vec_reg<Vector> vectorBatched0(
      clk, rst, en0, vectorData0, vectorInit0, vectorBatchedQ0);
  pyc::cpp::pyc_vec_reg<Vector> vectorBatched1(
      clk, rst, en1, vectorData1, vectorInit1, vectorBatchedQ1);
  previousClock = false;
  for (unsigned iteration = 0; iteration < 30000; ++iteration) {
    std::uint32_t controls = next();
    clk = Wire<1>(controls & 1u);
    rst = Wire<1>((controls >> 1) & 1u);
    en0 = Wire<1>((controls >> 2) & 1u);
    en1 = Wire<1>((controls >> 3) & 1u);
    for (unsigned lane = 0; lane < 2; ++lane) {
      vectorData0[lane] = Wire<8>(next() & 255u);
      vectorData1[lane] = Wire<8>(next() & 255u);
    }
    vectorReference0.tick_compute();
    vectorReference1.tick_compute();
    bool clockNow = clk.toBool();
    bool edge = !previousClock && clockNow;
    previousClock = clockNow;
    if (edge) {
      if (rst.toBool()) {
        vectorBatched0.posedge_reset_compute();
        vectorBatched1.posedge_reset_compute();
      } else {
        vectorBatched0.posedge_data_compute();
        vectorBatched1.posedge_data_compute();
      }
    } else if (clockNow) {
      vectorBatched0.noedge_update();
      vectorBatched1.noedge_update();
    } else {
      vectorBatched0.negedge_update();
      vectorBatched1.negedge_update();
    }
    if (vectorReference0.pending != vectorBatched0.pending ||
        vectorReference1.pending != vectorBatched1.pending ||
        vectorReference0.clkPrev != vectorBatched0.clkPrev ||
        vectorReference1.clkPrev != vectorBatched1.clkPrev ||
        vectorReference0.qNext != vectorBatched0.qNext ||
        vectorReference1.qNext != vectorBatched1.qNext)
      return 3;
    if (next() & 1u) {
      vectorReference0.tick_commit();
      vectorReference1.tick_commit();
      vectorBatched0.tick_commit();
      vectorBatched1.tick_commit();
      if (vectorReferenceQ0 != vectorBatchedQ0 ||
          vectorReferenceQ1 != vectorBatchedQ1)
        return 4;
    }
  }
  return 0;
}
