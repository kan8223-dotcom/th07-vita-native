#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_spritereuse {
// PushSprite emits 0,1,2,1,2,3. Match full immutable input bytes before
// reusing transformed vertices; unrelated triangle lists take the fallback.
inline bool matches(const unsigned char* source,std::size_t index,std::size_t stride)
{
    const unsigned slot=unsigned(index%6);
    return (slot==3||slot==4)&&std::memcmp(source+index*stride,source+(index-2)*stride,stride)==0;
}
template<std::size_t Bytes>
inline bool matchesFixed(const unsigned char* source,std::size_t index)
{
    static_assert(Bytes%4==0,"vertex input words");
    const unsigned slot=unsigned(index%6);
    if(slot!=3&&slot!=4)return false;
    std::uint32_t a[Bytes/4],b[Bytes/4],different=0;
    std::memcpy(a,source+index*Bytes,Bytes);
    std::memcpy(b,source+(index-2)*Bytes,Bytes);
    for(unsigned word=0;word<Bytes/4;++word)different|=a[word]^b[word];
    return different==0;
}
}
