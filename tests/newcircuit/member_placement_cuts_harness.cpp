#include "top.hpp"
#include <cstdint>
#include <cstdio>

template<class T> static T oracle(T value) {
  T twice = T(value + value);
  T mixed = T(twice ^ value);
  return T(T(mixed + value) ^ value);
}
int main() {
  pyc::gen::top sim;
  std::uint64_t digest=1469598103934665603ull;
  std::uint32_t random=0x81237eabu;
  auto next=[&]() { random^=random<<13;random^=random>>17;random^=random<<5;return random; };
  for(unsigned i=0;i<2048;++i) {
    std::uint8_t a=next(); std::uint16_t b=next();
    std::uint64_t c=(std::uint64_t(next())<<32)|next();
    std::uint64_t lo=i<128?~std::uint64_t(i):(std::uint64_t(next())<<32)|next();
    std::uint64_t hi=(std::uint64_t(next())<<32)|next();
    using U128=unsigned __int128;
    U128 d=(U128(hi)<<64)|lo, expected=oracle(d);
    sim.a=pyc::cpp::Wire<8>(a);sim.b=pyc::cpp::Wire<16>(b);sim.c=pyc::cpp::Wire<64>(c);
    sim.d.setWord(0,lo);sim.d.setWord(1,hi);
    for(unsigned repeat=0;repeat<3;++repeat) {
      sim.eval();
      if(sim.oa.value()!=oracle(a)||sim.ob.value()!=oracle(b)||sim.oc.value()!=oracle(c)||
         sim.od.word(0)!=std::uint64_t(expected)||sim.od.word(1)!=std::uint64_t(expected>>64)) return 1;
    }
    for(auto value:{std::uint64_t(oracle(a)),std::uint64_t(oracle(b)),oracle(c),std::uint64_t(expected),std::uint64_t(expected>>64)})
      digest=(digest^value)*1099511628211ull;
  }
  std::printf("Weighted locality oracle PASS: 2048 cases; digest=%016llx\n",static_cast<unsigned long long>(digest));
}
