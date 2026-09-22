#include "pyc_change_scheduler.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

using pyc::cpp::DirtyBitset;
using pyc::cpp::PublishResult;
using pyc::cpp::Version;
using pyc::cpp::VersionAdvance;
using pyc::cpp::VersionClock;
using pyc::cpp::VersionStamp;
using pyc::cpp::Wire;
using pyc::cpp::publishIfChanged;

namespace {

void testDirtyBitset() {
  DirtyBitset<130> dirty;
  static_assert(sizeof(dirty) == 3 * sizeof(std::uint64_t));

  std::size_t rank = 999;
  assert(dirty.empty());
  assert(!dirty.takeNext(rank));
  assert(rank == 999);

  assert(dirty.mark(65));
  assert(!dirty.mark(65));
  assert(dirty.mark(129));
  assert(dirty.mark(0));
  assert(dirty.mark(64));

  assert(dirty.takeNext(rank) && rank == 0);
  assert(dirty.takeNext(rank) && rank == 64);
  assert(dirty.takeNext(rank) && rank == 65);
  assert(dirty.takeNext(rank) && rank == 129);
  assert(dirty.empty());
  assert(!dirty.takeNext(rank));

  assert(dirty.mark(7));
  dirty.clear();
  assert(dirty.empty());
}

template <unsigned Width>
void expectPublishBehavior(Wire<Width> initial, Wire<Width> changed) {
  VersionClock clock;
  Wire<Width> destination = initial;

  assert(publishIfChanged(destination, initial, clock) ==
         PublishResult::unchanged);
  assert(clock.stamp() == VersionStamp{});
  assert(destination == initial);

  assert(publishIfChanged(destination, changed, clock) ==
         PublishResult::changed);
  assert((clock.stamp() == VersionStamp{0, 1}));
  assert(destination == changed);

  assert(publishIfChanged(destination, changed, clock) ==
         PublishResult::unchanged);
  assert((clock.stamp() == VersionStamp{0, 1}));
}

void testSemanticPublish() {
  expectPublishBehavior(Wire<8>(0x12), Wire<8>(0x34));
  expectPublishBehavior(Wire<64>(0x0123456789abcdefULL),
                        Wire<64>(0xfedcba9876543210ULL));

  Wire<130> wideA;
  wideA.setWord(0, 0x0123456789abcdefULL);
  wideA.setWord(1, 0xfedcba9876543210ULL);
  wideA.setWord(2, 0x1);
  Wire<130> wideB = wideA;
  wideB.setWord(1, 0x1111111111111111ULL);
  expectPublishBehavior(wideA, wideB);
}

void testVersionWrapAndRebase() {
  const Version max = std::numeric_limits<Version>::max();

  VersionClock wrapping({7, max});
  assert(wrapping.advance() == VersionAdvance::advanced);
  assert((wrapping.stamp() == VersionStamp{8, 0}));
  assert(wrapping.advance() == VersionAdvance::advanced);
  assert((wrapping.stamp() == VersionStamp{8, 1}));

  VersionClock exhausted({max, max});
  Wire<8> destination(1);
  assert(publishIfChanged(destination, Wire<8>(2), exhausted) ==
         PublishResult::rebaseRequired);
  assert(destination == Wire<8>(1));
  assert((exhausted.stamp() == VersionStamp{max, max}));

  bool invalidated = false;
  exhausted.rebase([&] { invalidated = true; });
  assert(invalidated);
  assert(exhausted.stamp() == VersionStamp{});
  assert(publishIfChanged(destination, Wire<8>(2), exhausted) ==
         PublishResult::changed);
  assert(destination == Wire<8>(2));
  assert((exhausted.stamp() == VersionStamp{0, 1}));
}

} // namespace

int main() {
  testDirtyBitset();
  testSemanticPublish();
  testVersionWrapAndRebase();
  return 0;
}
