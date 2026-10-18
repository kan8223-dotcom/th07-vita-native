#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace th07_viewinside {
inline bool inside(const float* position)
{
    std::uint32_t w;std::memcpy(&w,position+3,4);
    if(w==0x3f800000u){
        std::uint32_t xy[2];std::memcpy(xy,position,8);
        return (xy[0]&0x7fffffffu)<=0x3f800000u&&(xy[1]&0x7fffffffu)<=0x3f800000u;
    }
    return position[3]>0&&std::fabs(position[0])<=position[3]&&std::fabs(position[1])<=position[3];
}
}
