#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
#include <arm_neon.h>
#endif
namespace th07_textbox {
// Exact existing box average for unclipped horizontal 2x spans. Bounds and
// formats are checked by the SDL caller. All sums fit in unsigned 16 bits.
inline void row(const std::uint8_t* source,int sourcePitch,int firstX,int firstY,
                int sampleRows,int width,std::uint8_t* destination)
{
    int x=0;
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
    for(;x+8<=width;x+=8){
        auto red=vdupq_n_u16(0),green=red,blue=red,alpha=red;
        for(int y=0;y<sampleRows;++y){
            const auto pixels=vld4q_u8(source+std::size_t(firstY+y)*sourcePitch+(firstX+x*2)*4);
            red=vaddq_u16(red,vpaddlq_u8(pixels.val[0]));
            green=vaddq_u16(green,vpaddlq_u8(pixels.val[1]));
            blue=vaddq_u16(blue,vpaddlq_u8(pixels.val[2]));
            alpha=vaddq_u16(alpha,vpaddlq_u8(pixels.val[3]));
        }
        uint8x8x4_t output;
        const auto average=[&](uint16x8_t sum){
            uint16x8_t average;
            if(sampleRows==1)average=vshrq_n_u16(sum,1);
            else if(sampleRows==2)average=vshrq_n_u16(sum,2);
            else if(sampleRows==4)average=vshrq_n_u16(sum,3);
            else{
                // ceil(65536/6). For sum in [0,6*255], this integer
                // reciprocal has exactly the same floor as division by 6.
                const auto low=vmull_n_u16(vget_low_u16(sum),10923);
                const auto high=vmull_n_u16(vget_high_u16(sum),10923);
                average=vcombine_u16(vshrn_n_u32(low,16),vshrn_n_u32(high,16));
            }
            return vmovn_u16(average);
        };
        output.val[0]=average(red);output.val[1]=average(green);
        output.val[2]=average(blue);output.val[3]=average(alpha);
        vst4_u8(destination+x*4,output);
    }
#endif
    for(;x<width;++x)for(unsigned c=0;c<4;++c){
        unsigned sum=0;
        for(int y=0;y<sampleRows;++y){
            const auto* pixel=source+std::size_t(firstY+y)*sourcePitch+(firstX+x*2)*4;
            sum+=pixel[c]+unsigned(pixel[c+4]);
        }
        destination[x*4+c]=std::uint8_t(sum/unsigned(sampleRows*2));
    }
}
}
