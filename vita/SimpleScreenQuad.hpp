#pragma once
#include <cstdint>
#include <cstring>
#include "SpriteQuadPacking.hpp"
#include "QuadAxisReuse.hpp"
namespace th07_simplescreen {
// Called only after all four raw source colours are proven equal and fog is
// disabled. The source remains snapshotted; no game/RNG data is consulted.
template<class Vertex,class Decode>
inline void convert(Vertex* out,const void* data,unsigned count,int stride,bool textured,
                    float viewX,float viewY,float width,float height,const float* constantArg,
                    bool rgbArg,bool alphaArg,const float* identityFog,Decode decode){
    const auto* source=static_cast<const std::uint8_t*>(data);
    for(unsigned q=0;q<count;q+=4){
        float decoded[4];
        if(constantArg)std::memcpy(decoded,constantArg,16);
        else{std::uint32_t colour;std::memcpy(&colour,source+std::size_t(q)*stride+16,4);decode(colour,decoded);}
        float diffuse[4]={rgbArg?decoded[0]:1,rgbArg?decoded[1]:1,rgbArg?decoded[2]:1,alphaArg?decoded[3]:1};
        Vertex* quad=out+(q/4)*6;
#if defined(TH07_QUAD_AXIS_REUSE) && TH07_QUAD_AXIS_REUSE
        float positions[4][3],xs[4],ys[4],screenXs[4],screenYs[4];
        for(unsigned k=0;k<4;++k){std::memcpy(positions[k],source+std::size_t(q+k)*stride,12);xs[k]=positions[k][0];ys[k]=positions[k][1];}
        th07_quadaxis::transform4(xs,screenXs,[&](float x){return (x-viewX)*2/width-1;});
        th07_quadaxis::transform4(ys,screenYs,[&](float y){return 1-(y-viewY)*2/height;});
#endif
        for(unsigned k=0;k<4;++k){
            constexpr unsigned slot[4]={0,1,2,5};Vertex& v=quad[slot[k]];
            const auto* src=source+std::size_t(q+k)*stride;
#if defined(TH07_QUAD_AXIS_REUSE) && TH07_QUAD_AXIS_REUSE
            const float* position=positions[k];v.position[0]=screenXs[k];v.position[1]=screenYs[k];
#else
            float position[3];std::memcpy(position,src,12);
            v.position[0]=(position[0]-viewX)*2/width-1;
            v.position[1]=1-(position[1]-viewY)*2/height;
#endif
            v.position[2]=position[2];v.position[3]=1;
            if(textured)std::memcpy(v.uv,src+20,8);else v.uv[0]=v.uv[1]=0;
            std::memcpy(v.diffuse,diffuse,16);std::memcpy(v.fog,identityFog,16);
        }
        th07_spritequad::finishExpanded(quad);
    }
}
}
