#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
namespace th07_plainruns {
struct Box {float x0=1e30f,y0=1e30f,x1=-1e30f,y1=-1e30f;bool unbounded=false;};
inline bool disjoint(const Box& a,const Box& b){
    // More than two native pixels of separation, also covering raster
    // coordinate rounding. Eye-plane/nonfinite/non-screen geometry blocks
    // movement. This predicate never claims touching boxes are disjoint.
    constexpr float padding=.01f;
    return !a.unbounded&&!b.unbounded&&(a.x1+padding<b.x0||b.x1+padding<a.x0||a.y1+padding<b.y0||b.y1+padding<a.y0);
}
struct Run {unsigned first=0,count=0;float rgba[4]{};Box box;};
struct Plan {
    std::vector<Run> runs;
    std::vector<unsigned> order;
    std::vector<std::uint8_t> used;
    unsigned moved=0;
    template<class Vertex>
    bool build(const Vertex* input,unsigned count){
        runs.clear();order.clear();moved=0;
        for(unsigned at=0;at+2<count;at+=3){
            if(runs.empty()||std::memcmp(runs.back().rgba,input[at].diffuse,16)){
                runs.emplace_back();runs.back().first=at;std::memcpy(runs.back().rgba,input[at].diffuse,16);
            }
            runs.back().count+=3;
        }
        if(runs.size()<8)return false;
        bool repeated=false;
        for(unsigned i=0;i<runs.size()&&!repeated;++i)
            for(unsigned j=i+1;j<std::min<unsigned>(runs.size(),i+17);++j)
                if(!std::memcmp(runs[i].rgba,runs[j].rgba,16)){repeated=true;break;}
        if(!repeated)return false;
        // Most title/background runs are a single tint or all different.
        // Do not project/bound thousands of vertices without a merge candidate.
        for(auto& r:runs){
            for(unsigned k=r.first;k<r.first+r.count;++k){
                const auto* p=input[k].position;
                if(p[3]!=1||!std::isfinite(p[0])||!std::isfinite(p[1])){r.box.unbounded=true;continue;}
                r.box.x0=std::min(r.box.x0,p[0]);r.box.x1=std::max(r.box.x1,p[0]);
                r.box.y0=std::min(r.box.y0,p[1]);r.box.y1=std::max(r.box.y1,p[1]);
            }
        }
        used.assign(runs.size(),0);
        for(unsigned i=0;i<runs.size();++i){
            if(used[i])continue;
            used[i]=1;order.push_back(i);
            const unsigned end=std::min<unsigned>(runs.size(),i+17);
            for(unsigned j=i+1;j<end;++j){
                if(used[j]||std::memcmp(runs[i].rgba,runs[j].rgba,16))continue;
                bool canMove=true;
                for(unsigned k=i+1;k<j&&canMove;++k)
                    if(!used[k]&&!disjoint(runs[j].box,runs[k].box))canMove=false;
                if(canMove){used[j]=1;order.push_back(j);++moved;}
            }
        }
        return moved!=0;
    }
};
}
