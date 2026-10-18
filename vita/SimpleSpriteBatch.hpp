#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_simplebatch {
// Native screen XYZRHW quads put their raw ARGB word at offset 16.
// Uniform words guarantee bit-identical decoded/combined diffuse values.
inline bool uniform(const void* source,int stride,unsigned vertices)
{
    if(!source||stride<20||vertices%4)return false;
    const auto* bytes=static_cast<const std::uint8_t*>(source);
    for(unsigned q=0;q<vertices;q+=4){
        std::uint32_t colour[4];
        for(unsigned i=0;i<4;++i)std::memcpy(&colour[i],bytes+std::size_t(q+i)*stride+16,4);
        if(colour[0]!=colour[1]||colour[0]!=colour[2]||colour[0]!=colour[3])return false;
    }
    return true;
}
}
