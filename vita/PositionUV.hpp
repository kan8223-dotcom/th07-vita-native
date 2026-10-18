#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_positionuv {
template<class Vertex>
void pack(void* destination,const Vertex* vertices,std::size_t count)
{
    auto* output=static_cast<std::uint8_t*>(destination);
    for(std::size_t i=0;i<count;++i){
        std::memcpy(output+i*24,vertices[i].position,16);
        std::memcpy(output+i*24+16,vertices[i].uv,8);
    }
}
}
