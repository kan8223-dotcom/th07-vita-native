#pragma once
#include <cstdint>
#include <array>
#include <cstddef>
#include "DepthRunWindow.hpp"
#if defined(TH07_DEPTH_RLE_MASK) && TH07_DEPTH_RLE_MASK
#include "DepthRunMask.hpp"
#endif
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
#include <arm_neon.h>
#endif
namespace th07_depthruns {
template<class Rows>
void buildRows(Rows& rows,const std::uint8_t* cells,int width,int height,unsigned threshold){
    rows.resize(height);
    for(int y=0;y<height;++y){
        auto& row=rows[y];row.clear();int begin=-1,x=0;
        const auto* source=cells+std::size_t(y)*width;
        for(;threshold<=255&&x+16<=width;x+=16){
#if defined(TH07_DEPTH_RLE_MASK) && TH07_DEPTH_RLE_MASK
            th07_runmask::append16(row,begin,x,th07_runmask::passing16(source+x,threshold));
#else
            bool allPass=true,allFail=true;
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
            const auto mask=vcgeq_u8(vld1q_u8(source+x),vdupq_n_u8(std::uint8_t(threshold)));
            const auto low=vget_low_u8(mask),high=vget_high_u8(mask);
            const auto both=vreinterpret_u32_u8(vand_u8(low,high)),either=vreinterpret_u32_u8(vorr_u8(low,high));
            allPass=(vget_lane_u32(both,0)&vget_lane_u32(both,1))==0xffffffffu;
            allFail=(vget_lane_u32(either,0)|vget_lane_u32(either,1))==0;
#else
            for(int k=0;k<16;++k){allPass&=source[x+k]>=threshold;allFail&=source[x+k]<threshold;}
#endif
            if(allPass){if(begin<0)begin=x;continue;}
            if(allFail){if(begin>=0){row.emplace_back(begin,x);begin=-1;}continue;}
            for(int k=x;k<x+16;++k){
                const bool pass=source[k]>=threshold;
                if(pass&&begin<0)begin=k;
                if(!pass&&begin>=0){row.emplace_back(begin,k);begin=-1;}
            }
#endif
        }
        for(;x<width;++x){
            const bool pass=source[x]>=threshold;
            if(pass&&begin<0)begin=x;
            if(!pass&&begin>=0){row.emplace_back(begin,x);begin=-1;}
        }
        if(begin>=0)row.emplace_back(begin,width);
    }
}
// Sum exactly the same passing cells as the 2-D prefix table. Early exit is
// allowed only once both answers are irrevocable (some pass, not all pass).
template<class Rows>
void classify(const Rows& rows,const int (*xr)[2],int nx,const int (*yr)[2],int ny,bool& any,bool& all){
    any=false;all=true;
    for(int r=0;r<ny;++r)for(int y=yr[r][0];y<yr[r][1];++y)for(int x=0;x<nx;++x){
        const int lo=xr[x][0],hi=xr[x][1];int covered=0;
#if defined(TH07_DEPTH_RUN_WINDOW) && TH07_DEPTH_RUN_WINDOW
        th07_runwindow::visit(rows[y],lo,hi,[&](const auto& run){
            const int first=run.first>lo?run.first:lo,last=run.second<hi?run.second:hi;
            if(last>first)covered+=last-first;
        });
#else
        for(const auto& run:rows[y]){
            if(run.second<=lo)continue;
            if(run.first>=hi)break;
            const int first=run.first>lo?run.first:lo,last=run.second<hi?run.second:hi;
            if(last>first)covered+=last-first;
        }
#endif
        any|=covered!=0;all&=covered==hi-lo;
        if(any&&!all)return;
    }
}
// Two additional atlases globally, not per texture or per alpha reference.
// The caller bounds atlas dimensions/storage and retains the old fallback.
template<class Data>
struct Cache {
    struct Entry {Data data;std::uint64_t used=0;};
    std::array<Entry,2> entries{};
    std::uint64_t tick=0;
    struct Selection {Data& data;bool hit;};
    Selection get(std::uint32_t version,std::uint32_t threshold){
        ++tick;
        for(auto& e:entries)if(e.data.version==version&&e.data.threshold==threshold){e.used=tick;return {e.data,true};}
        auto& e=entries[entries[0].used<=entries[1].used?0:1];
        e.used=tick;return {e.data,false};
    }
};
}
