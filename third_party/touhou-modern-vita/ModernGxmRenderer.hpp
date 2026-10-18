#pragma once
#include "GxmRenderer.hpp"
#include "ProvenVertex.hpp"
#include <array>
namespace touhou::modernvita {
using Vertex=touhou::vita::Vertex;
using DrawState=touhou::vita::DrawState;
using Position=std::array<float,4>;
using Colour=std::array<float,8>;
struct DepthBuffer;
// Isolated fork: do not change the common Device used by TH06.
class Device {
public:
    enum class Layout:uint8_t { Full,Position,Colour,UV,ColourUV,Proven };
    static constexpr unsigned stride(Layout layout){return layout==Layout::Full?72:layout==Layout::Position?16:layout==Layout::Colour?32:layout==Layout::UV?24:layout==Layout::Proven?56:40;}
    using Statistics=touhou::vita::Device::Statistics;
    Device();~Device();
    bool initialize(const char* directory="app0:/shaders");
    unsigned probePixelClip(); // bit 0 colour, bit 1 colour+depth (measured, not assumed)
    void enableColourClip(bool);bool colourClipActive()const;
    std::array<uint64_t,2> uniformCacheStats()const;
    const char* error()const;Statistics statistics()const;
    vita2d_texture* createTexture(uint32_t width,uint32_t height,bool target=false);
    bool upload(vita2d_texture*,const void*,uint32_t pitch,int x=0,int y=0,int w=-1,int h=-1);
    void destroyTexture(vita2d_texture*);
    DepthBuffer* createDepth(uint32_t,uint32_t,bool);void destroyDepth(DepthBuffer*);
    bool target(vita2d_texture*,DepthBuffer* depth=nullptr);
    void viewport(float,float,float,float,float min=0,float max=1);
    const Vertex* stage(const Vertex*,uint32_t);Vertex* reserve(uint32_t);
    bool draw(const Vertex*,uint32_t,vita2d_texture*,const DrawState&);
    bool drawStaged(const Vertex*,uint32_t,vita2d_texture*,const DrawState&);
    const ProvenVertex* stage(const ProvenVertex*,uint32_t);
    const void* stageProvenUV(const ProvenVertex*,uint32_t); // TH07-only, existing UV layout
    bool draw(const ProvenVertex*,uint32_t,vita2d_texture*,const DrawState&);
    bool drawStaged(const ProvenVertex*,uint32_t,vita2d_texture*,const DrawState&);
    const Position* stagePositions(const Position*,uint32_t);
    bool drawPositions(const Position*,uint32_t,const DrawState&);
    bool drawPositionsStaged(const Position*,uint32_t,const DrawState&);
    const Colour* stageColours(const Colour*,uint32_t);
    bool drawColours(const Colour*,uint32_t,const DrawState&);
    bool drawColoursStaged(const Colour*,uint32_t,const DrawState&);
    const void* stageCompact(const Vertex*,uint32_t,Layout);
    bool drawCompact(const Vertex*,uint32_t,vita2d_texture*,const DrawState&,Layout);
    bool drawCompactStaged(const void*,uint32_t,vita2d_texture*,const DrawState&,Layout);
    bool clear(bool,bool,const float[4],float,int x=0,int y=0,int w=-1,int h=-1);
    bool finish();bool present();bool read(vita2d_texture*,void*,uint32_t);
    uint32_t targetWidth()const;uint32_t targetHeight()const;
private:
    bool drawStream(const void*,uint32_t,vita2d_texture*,const DrawState&,Layout);
    struct Impl;Impl* impl;
};
}
