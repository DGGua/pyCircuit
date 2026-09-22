#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "pyc_bits.hpp"

namespace pyc::cpp {

// Fixed-capacity dirty set keyed by precomputed topological rank. Marking an
// already-dirty rank coalesces the duplicate, and takeNext always removes the
// smallest rank. All storage is inline; no operation allocates.
template <std::size_t Capacity>
class DirtyBitset {
  static_assert(Capacity > 0, "DirtyBitset requires non-zero capacity");

public:
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kWordBits = 64;
  static constexpr std::size_t kWords =
      (kCapacity + kWordBits - 1) / kWordBits;

  // Returns true only when rank transitions from clean to dirty.
  bool mark(std::size_t rank) noexcept {
    assert(rank < kCapacity && "dirty rank exceeds fixed capacity");
    if (rank >= kCapacity)
      return false;
    const std::size_t word = rank / kWordBits;
    const std::uint64_t mask = std::uint64_t{1} << (rank % kWordBits);
    const bool inserted = (words_[word] & mask) == 0;
    words_[word] |= mask;
    return inserted;
  }

  bool test(std::size_t rank) const noexcept {
    assert(rank < kCapacity && "dirty rank exceeds fixed capacity");
    if (rank >= kCapacity)
      return false;
    const std::size_t word = rank / kWordBits;
    const std::uint64_t mask = std::uint64_t{1} << (rank % kWordBits);
    return (words_[word] & mask) != 0;
  }

  // Removes one known rank. Returns true only if it was dirty.
  bool take(std::size_t rank) noexcept {
    assert(rank < kCapacity && "dirty rank exceeds fixed capacity");
    if (rank >= kCapacity)
      return false;
    const std::size_t word = rank / kWordBits;
    const std::uint64_t mask = std::uint64_t{1} << (rank % kWordBits);
    const bool present = (words_[word] & mask) != 0;
    words_[word] &= ~mask;
    return present;
  }

  // Removes the minimum topological rank. Returns false when the set is empty.
  bool takeNext(std::size_t &rank) noexcept {
    for (std::size_t word = 0; word < kWords; ++word) {
      if (words_[word] == 0)
        continue;
      const unsigned bit = lowestSetBit(words_[word]);
      words_[word] &= ~(std::uint64_t{1} << bit);
      rank = word * kWordBits + bit;
      return true;
    }
    return false;
  }

  bool empty() const noexcept {
    for (std::uint64_t word : words_) {
      if (word != 0)
        return false;
    }
    return true;
  }

  std::size_t count() const noexcept {
    std::size_t total = 0;
    for (std::uint64_t word : words_) {
      while (word != 0) {
        word &= word - 1;
        ++total;
      }
    }
    return total;
  }

  void clear() noexcept { words_.fill(0); }

private:
  static unsigned lowestSetBit(std::uint64_t word) noexcept {
    assert(word != 0);
    unsigned bit = 0;
    while ((word & std::uint64_t{1}) == 0) {
      word >>= 1;
      ++bit;
    }
    return bit;
  }

  std::array<std::uint64_t, kWords> words_{};
};

using Version = std::uint64_t;
using Epoch = std::uint64_t;

struct VersionStamp {
  Epoch epoch = 0;
  Version version = 0;

  friend constexpr bool operator==(VersionStamp lhs,
                                   VersionStamp rhs) noexcept {
    return lhs.epoch == rhs.epoch && lhs.version == rhs.version;
  }

  friend constexpr bool operator!=(VersionStamp lhs,
                                   VersionStamp rhs) noexcept {
    return !(lhs == rhs);
  }
};

enum class VersionAdvance {
  advanced,
  rebaseRequired,
};

// A version wraps into a new epoch without aliasing old stamps. Exhausting both
// counters requires a coordinated rebase so all cached stamps are invalidated
// before the clock restarts.
class VersionClock {
public:
  constexpr VersionClock() = default;
  explicit constexpr VersionClock(VersionStamp initial) : stamp_(initial) {}

  constexpr VersionStamp stamp() const noexcept { return stamp_; }

  VersionAdvance advance() noexcept {
    if (stamp_.version != std::numeric_limits<Version>::max()) {
      ++stamp_.version;
      return VersionAdvance::advanced;
    }
    if (stamp_.epoch == std::numeric_limits<Epoch>::max())
      return VersionAdvance::rebaseRequired;
    ++stamp_.epoch;
    stamp_.version = 0;
    return VersionAdvance::advanced;
  }

  // invalidateCachedStamps must invalidate every consumer snapshot associated
  // with this clock. Requiring the callback makes the otherwise unsafe global
  // reset explicit at the call site.
  template <typename InvalidateFn>
  void rebase(InvalidateFn &&invalidateCachedStamps) {
    std::forward<InvalidateFn>(invalidateCachedStamps)();
    stamp_ = {};
  }

private:
  VersionStamp stamp_{};
};

enum class PublishResult {
  unchanged,
  changed,
  rebaseRequired,
};

// Publish a port value and bump its version only on semantic change.
//
// Wire is currently two-valued, so equality deliberately delegates to
// Wire::operator==. When the dialect/runtime gain X/Z storage, that operator
// must implement the full (value_bits, known_mask, z_mask) equality contract
// before this helper can provide four-valued semantic change detection.
template <unsigned Width>
PublishResult publishIfChanged(Wire<Width> &destination,
                               const Wire<Width> &next,
                               VersionClock &clock) {
  if (destination == next)
    return PublishResult::unchanged;
  if (clock.advance() == VersionAdvance::rebaseRequired)
    return PublishResult::rebaseRequired;
  destination = next;
  return PublishResult::changed;
}

} // namespace pyc::cpp
