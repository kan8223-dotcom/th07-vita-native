#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_depthfan {
// A triangulated polygon emits 0,1,2, 0,2,3, 0,3,4,... . Metadata comes
// from that exact fan emitter; no floating-point equality checks are needed.
template<class Position,class Compute>
std::size_t expand(Position* output,std::size_t count,const std::uint8_t* fans,
                   std::size_t fanCount,Compute&& compute)
{
    std::size_t at=0,reused=0;
    for(std::size_t f=0;f<fanCount;++f){
        const unsigned points=fans[f];assert(points>=3&&points<=32);
        const std::size_t vertices=(points-2)*3;
        assert(at+vertices<=count);
        compute(at);compute(at+1);compute(at+2);
        for(unsigned triangle=1;triangle+2<points;++triangle){
            const auto next=at+triangle*3;
            std::memcpy(output[next].position,output[at].position,4*sizeof(float));
            std::memcpy(output[next+1].position,output[next-1].position,4*sizeof(float));
            compute(next+2);reused+=2;
        }
        at+=vertices;
    }
    assert(at==count);return reused;
}
}
