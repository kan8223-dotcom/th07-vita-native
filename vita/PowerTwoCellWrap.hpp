#pragma once
#include <cassert>
#include <cstdint>
namespace th07_cellwrap {
inline unsigned remainder(int value,unsigned extent){
    assert(extent&&!(extent&(extent-1)));return std::uint32_t(value)&(extent-1);
}
inline int floorTile(int value,unsigned extent){
    assert(extent&&!(extent&(extent-1)));const unsigned shift=__builtin_ctz(extent);
    return value>=0?int(std::uint32_t(value)>>shift):-1-int(std::uint32_t(-1-value)>>shift);
}
}
