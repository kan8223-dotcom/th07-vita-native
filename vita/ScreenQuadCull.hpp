#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_screenquad {
enum class Result {Unprepared,Prepared,Outside};
inline Result prepare(const void* source,int stride,float x,float y,float width,float height,float (&xy)[4][2])
{
    const auto* bytes=static_cast<const std::uint8_t*>(source);float first[2];std::memcpy(first,bytes,8);
    // Most visible quads need no early check. This is conservative: the
    // original per-triangle rejection still runs on all retained geometry.
    if(first[0]>=x&&first[0]<=x+width&&first[1]>=y&&first[1]<=y+height)return Result::Unprepared;
    for(unsigned i=0;i<4;++i){
        float position[2];std::memcpy(position,bytes+std::size_t(i)*stride,8);
        xy[i][0]=(position[0]-x)*2/width-1;
        xy[i][1]=1-(position[1]-y)*2/height;
    }
    for(unsigned i=0;i<4;++i)
        if(std::fabs(xy[i][0])<=1&&std::fabs(xy[i][1])<=1)return Result::Prepared;
    for(unsigned plane=0;plane<4;++plane){
        float distance[4],magnitude=1;
        for(unsigned i=0;i<4;++i){
            distance[i]=plane==0?1+xy[i][0]:plane==1?1-xy[i][0]:plane==2?1+xy[i][1]:1-xy[i][1];
            magnitude+=std::fabs(distance[i]);
        }
        // Same margin/operations, but all four corners. The nonnegative sum
        // is >= each original three-corner sum, so rejection implies BOTH
        // original triangles (0,1,2 and 1,2,3) were rejected as well.
        const float margin=1e-3f*magnitude;
        if(distance[0]<-margin&&distance[1]<-margin&&distance[2]<-margin&&distance[3]<-margin)return Result::Outside;
    }
    return Result::Prepared;
}
}
