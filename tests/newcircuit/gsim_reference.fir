FIRRTL version 3.3.0
circuit SmallReference :
  module SmallReference :
    input a : UInt<16>
    input b : UInt<16>
    input c : UInt<16>
    input sel : UInt<1>
    output sum16 : UInt<16>
    output sub16 : UInt<16>
    output product16 : UInt<16>
    output selected : UInt<16>
    output low8 : UInt<8>
    output middle8 : UInt<8>
    output crossing8 : UInt<8>

    node add17 = add(a, b)
    node sub17 = sub(a, b)
    node product32 = mul(a, b)
    node sumlow = bits(add17, 15, 0)
    node chosen = mux(sel, sumlow, xor(a, c))
    node joined = cat(a, b)
    connect sum16, sumlow
    connect sub16, bits(sub17, 15, 0)
    connect product16, bits(product32, 15, 0)
    connect selected, chosen
    connect low8, bits(chosen, 7, 0)
    connect middle8, bits(product32, 15, 8)
    connect crossing8, bits(joined, 19, 12)
