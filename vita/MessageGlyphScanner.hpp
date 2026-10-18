#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace th07_msgglyphs {
inline std::uint32_t read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);
}
inline unsigned read16(const std::uint8_t* p){return unsigned(p[0])|(unsigned(p[1])<<8);}
struct Result {unsigned messages=0;bool complete=false;};
// Read only the selected character's bounded MSG entries. No VM is created,
// no instruction is executed, and callbacks only receive NUL-terminated text.
template<class Warm>
Result scan(const void* messageFile,std::size_t bytes,int character,Warm&& warm)
{
    Result result;
    if(!messageFile||bytes<4||bytes>(4u<<20)||character<0||character>2)return result;
    const auto* file=static_cast<const std::uint8_t*>(messageFile);
    const auto count=read32(file);
    if(!count||count>4096||count>(bytes-4)/4)return result;
    const std::size_t header=4+std::size_t(count)*4;
    for(unsigned entry=0;entry<count;++entry){
        const auto offset=read32(file+4+entry*4);
        if(offset<header||offset>bytes)return result;
    }
    const unsigned first=unsigned(character)*10,last=std::min(count,first+10);
    if(first>=last)return result;
    for(unsigned entry=first;entry<last;++entry){
        std::size_t cursor=read32(file+4+entry*4),end=bytes;
        for(unsigned other=0;other<count;++other){
            const auto offset=read32(file+4+other*4);
            if(offset>cursor&&offset<end)end=offset;
        }
        bool terminated=false;
        while(cursor<end&&end-cursor>=4){
            const unsigned opcode=file[cursor+2],args=file[cursor+3];
            const std::size_t instructionBytes=4+args;
            if(instructionBytes>end-cursor)return result;
            if(opcode==0){terminated=true;break;}
            if(opcode==3||opcode==8){
                if(args<5||read16(file+cursor+4)>=2||read16(file+cursor+6)>=2)return result;
                const char* text=reinterpret_cast<const char*>(file+cursor+8);
                if(!std::memchr(text,0,args-4))return result;
                if(opcode==3){warm(text);++result.messages;}
            }
            cursor+=instructionBytes;
        }
        if(!terminated)return result;
    }
    result.complete=true;return result;
}
}
