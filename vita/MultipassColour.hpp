#pragma once
#include <array>
#include <cstring>
namespace th07_multipass {
// The auxiliary passes use only position and diffuse in the three proven
// fragment programs. Generate the existing 32-byte colour stream directly.
template<class Vertex>
std::array<float,8> colour(const Vertex& v,unsigned kind){
    std::array<float,8> out;
    std::memcpy(out.data(),v.position,16);
    const float f=v.fog[3];
    if(kind==0||kind==2){
        out[4]=out[5]=out[6]=0;
        out[7]=kind==0?f:v.diffuse[3];
    }else{
        for(unsigned c=0;c<3;++c)out[4+c]=v.fog[c]*(1-f);
        out[7]=kind==1?1:f;
    }
    return out;
}
}
