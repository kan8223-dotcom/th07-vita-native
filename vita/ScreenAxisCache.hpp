#pragma once
#include <cstdint>
#include <cstring>
namespace th07_screenaxis {
struct Cache {
    std::uint32_t keys[2]{};float results[2]{};unsigned valid=0;
    unsigned hits=0,misses=0;
    template<class Compute>
    float get(float input,Compute&& compute){
        std::uint32_t key;std::memcpy(&key,&input,4);
        for(unsigned i=0;i<valid;++i)if(keys[i]==key){++hits;return results[i];}
        ++misses;const float result=compute(input);
        if(valid){keys[1]=keys[0];results[1]=results[0];}
        keys[0]=key;results[0]=result;if(valid<2)++valid;return result;
    }
};
}
