#pragma once
#include <cstdint>
namespace th07_vertexcolour {
struct Cache {
    std::uint32_t previous=0;
    float rgba[4]{};
    bool valid=false;
    template<class Decoder>
    const float* get(std::uint32_t color,Decoder decode){
        if(!valid||color!=previous){decode(color,rgba);previous=color;valid=true;}
        return rgba;
    }
};
}
