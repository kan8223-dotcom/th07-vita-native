#pragma once
#include <cstdint>
#include <cstring>
namespace th07_quadaxis {
inline std::uint32_t bits(float v){std::uint32_t word;std::memcpy(&word,&v,4);return word;}
// Four exact raw inputs, not a searched coordinate cache. Reuse is legal even
// for NaN/-0: the old operation receives precisely the same float bits.
template<class Transform>
inline void transform4(const float (&input)[4],float (&out)[4],Transform transform){
    if(bits(input[0])==bits(input[2])&&bits(input[1])==bits(input[3])){
        out[0]=transform(input[0]);out[1]=transform(input[1]);out[2]=out[0];out[3]=out[1];
    }else if(bits(input[0])==bits(input[1])&&bits(input[2])==bits(input[3])){
        out[0]=transform(input[0]);out[2]=transform(input[2]);out[1]=out[0];out[3]=out[2];
    }else for(unsigned k=0;k<4;++k)out[k]=transform(input[k]);
}
}
