#pragma once
#include <cstdint>
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
#include <arm_neon.h>
#endif
namespace th07_runmask {
inline std::uint32_t passing16(const std::uint8_t* source,unsigned threshold){
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
    static const std::uint8_t weights[16]={1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
    const auto mask=vcgeq_u8(vld1q_u8(source),vdupq_n_u8(std::uint8_t(threshold)));
    const auto sum=vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(vandq_u8(mask,vld1q_u8(weights)))));
    return std::uint32_t(vgetq_lane_u64(sum,0))|(std::uint32_t(vgetq_lane_u64(sum,1))<<8);
#else
    std::uint32_t mask=0;
    for(unsigned k=0;k<16;++k)mask|=std::uint32_t(source[k]>=threshold)<<k;
    return mask;
#endif
}
// Every bit transition is exactly a scalar pass/fail state change. Keep an
// open run across blocks, including all-pass/all-fail and row tails.
template<class Row>
inline void append16(Row& row,int& begin,int x,std::uint32_t mask){
    std::uint32_t changes=(mask^(mask<<1))&0xffffu;
    if(begin>=0)changes^=1;
    while(changes){
        const int at=x+__builtin_ctz(changes);
        if(begin>=0){row.emplace_back(begin,at);begin=-1;}
        else begin=at;
        changes&=changes-1;
    }
}
}
