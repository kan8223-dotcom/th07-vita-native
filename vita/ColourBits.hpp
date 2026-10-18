#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_colourbits {
template<unsigned Words>
inline bool equal(const void* a,const void* b)
{
    static_assert(Words==3||Words==4,"RGB or RGBA only");
    if(a==b)return true;
    std::uint32_t left[Words],right[Words],difference=0;
    std::memcpy(left,a,sizeof(left));std::memcpy(right,b,sizeof(right));
    for(unsigned i=0;i<Words;++i)difference|=left[i]^right[i];
    return difference==0;
}
}
