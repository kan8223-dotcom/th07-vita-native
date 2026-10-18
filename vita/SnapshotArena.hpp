#pragma once
#include "ScratchSpan.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
namespace th07_snapshot {
struct Arena {
    std::vector<std::uint8_t> bytes;
    std::size_t used=0;
    static constexpr std::size_t limit=64u<<20;
    static constexpr std::size_t invalid=std::numeric_limits<std::size_t>::max();
    std::size_t append(const void* source,std::size_t count){
        if(!source||!count)return invalid;
        if(used>limit-3)return invalid;
        const auto first=(used+3)&~std::size_t(3);
        if(count>limit-first)return invalid;
        if(bytes.size()<first+count)bytes.resize(first+count);
        std::memcpy(bytes.data()+first,source,count);used=first+count;return first;
    }
    void reset(){used=0;} // high-water bytes/capacity stay owned by this slot
    const std::uint8_t* at(std::size_t offset)const{return bytes.data()+offset;}
};
}
