#pragma once
#include <cassert>
#include <cstddef>
#include <utility>
#if defined(TH07_BARY_SCALAR_BITS) && TH07_BARY_SCALAR_BITS
#include "ScalarBaryClassification.hpp"
#endif
#if defined(TH07_BARY_INTERPOLATE_NEON) && TH07_BARY_INTERPOLATE_NEON
#include "BaryInterpolation.hpp"
#endif
#if defined(TH07_BARY_PACKED_FLAGS) && TH07_BARY_PACKED_FLAGS
#include "PackedBaryClip.hpp"
#endif
#if defined(TH07_BARY_CLASSIFY_NEON) && TH07_BARY_CLASSIFY_NEON
#include "BaryClassification.hpp"
#endif
namespace th07_baryclip {
// Same walk/interpolation/order as the original cell-rectangle clip. A plane
// keeping all vertices cannot change the polygon; a plane rejecting every
// vertex cannot contribute any output. NaNs retain the original mixed walk.
template<class Bary,int Axis=-1>
void plane(Bary*& rect,Bary*& scratch,std::size_t& count,int axis,float edge){
    if constexpr(Axis>=0)axis=Axis;
    assert(count<=16); // four input corners, at most three half-plane clips
    bool allInside=true,allOutside=true;
#if defined(TH07_BARY_CLASSIFY_NEON) && TH07_BARY_CLASSIFY_NEON
    std::uint32_t pass[32];
    th07_baryclass::classify(rect,count,axis,edge,pass,allInside,allOutside);
#else
    bool pass[32];
#if defined(TH07_BARY_SCALAR_BITS) && TH07_BARY_SCALAR_BITS
    if(th07_scalarbary::bits(edge)==0xb951b717u){
        for(std::size_t i=0;i<count;++i){bool outside;th07_scalarbary::classify(rect[i].b[axis],pass[i],outside);
            allInside&=pass[i];allOutside&=outside;}
    }else
#endif
    for(std::size_t i=0;i<count;++i){
        pass[i]=rect[i].b[axis]>=edge;
        allInside&=pass[i];allOutside&=rect[i].b[axis]<edge;
    }
#endif
    if(allInside)return;
    if(allOutside){count=0;return;}
    std::size_t out=0;
    for(std::size_t i=0;i<count;++i){
        const std::size_t j=i+1==count?0:i+1;
        const Bary& p=rect[i];const Bary& q=rect[j];
        if(pass[i])scratch[out++]=p;
        if(pass[i]!=pass[j]){
            const float f=(p.b[axis]-edge)/(p.b[axis]-q.b[axis]);
            Bary m;
#if defined(TH07_BARY_INTERPOLATE_NEON) && TH07_BARY_INTERPOLATE_NEON
            th07_baryinterpolation::interpolate(m.b,p.b,q.b,f);
#else
            for(int c=0;c<3;++c)m.b[c]=p.b[c]+(q.b[c]-p.b[c])*f;
#endif
            m.b[axis]=edge;scratch[out++]=m;
        }
    }
    assert(out<=32);std::swap(rect,scratch);count=out;
}
template<class Bary>
void triangle(Bary*& rect,Bary*& scratch,std::size_t& count,float edge){
#if defined(TH07_BARY_PACKED_FLAGS) && TH07_BARY_PACKED_FLAGS
    th07_packedclip::triangle(rect,scratch,count,edge);return;
#endif
    // Same plane order, with constant axes available to the inliner instead
    // of a dynamic component index in the innermost rectangle walk.
#if defined(TH07_BARY_CLASSIFY_NEON) && TH07_BARY_CLASSIFY_NEON
    if(count)plane<Bary,0>(rect,scratch,count,0,edge);
    if(count)plane<Bary,1>(rect,scratch,count,1,edge);
    if(count)plane<Bary,2>(rect,scratch,count,2,edge);
#else
    if(count)plane(rect,scratch,count,0,edge);
    if(count)plane(rect,scratch,count,1,edge);
    if(count)plane(rect,scratch,count,2,edge);
#endif
}
}
