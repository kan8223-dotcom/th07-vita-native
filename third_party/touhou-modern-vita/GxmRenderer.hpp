#pragma once
#include <cstddef>
#include <cstdint>
#include <psp2/gxm.h>
#include <vita2d.h>

// A native ARM/GXM draw core. No PSP, ME, RSX or desktop GL dependency.
// Clip coordinates are homogeneous; never divide by W on the CPU. GXM uses
// 0 <= Z <= W. Adapters must convert an OpenGL projection's Z range once.
namespace touhou::vita {
struct DepthBuffer;
struct Vertex {
    float position[4];
    float diffuse[4];
    float uv[2];
    float specular[4];
    float fog[4]; // RGB and UNCLAMPED fog factor; clamp after interpolation.
};
struct DrawState {
    SceGxmPrimitiveType primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    SceGxmDepthFunc depthFunc = SCE_GXM_DEPTH_FUNC_ALWAYS;
    bool depthWrite = false;
    SceGxmCullMode cull = SCE_GXM_CULL_NONE;
    bool blend = true;
    SceGxmBlendFunc colorBlend = SCE_GXM_BLEND_FUNC_ADD;
    SceGxmBlendFunc alphaBlend = SCE_GXM_BLEND_FUNC_ADD;
    SceGxmBlendFactor colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    SceGxmBlendFactor colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    SceGxmBlendFactor alphaSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    SceGxmBlendFactor alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uint32_t colorMask = SCE_GXM_COLOR_MASK_ALL;
    // Modern GraphicsState enum numbering: RGB op, alpha op, RGB args 1/2;
    // alpha args 1/2. Source: diffuse=0, texel=1, factor=2, specular=3;
    // add 16 for complement and 32 for alpha replication.
    float operations[4] = {3, 3, 1, 0};
    float arguments[4] = {1, 0, 0, 0};
    float factor[4] = {1, 1, 1, 1};
    float alphaTest[4] = {0, 7, 0, 0}; // reference [0,255], Compare enum
    bool linear = true;
    // Which fragment program draws: the combine program (shaders/draw_f.gxp, the
    // codes above), or one of the three small programs a real Vita has run
    // correctly with draw_v (shaders/proven_*.gxp, the v5 diagnostic's stages
    // 4-6): a uniform color, the interpolated diffuse, or texel * tint. Their
    // only input besides the texture is `tint`; no alpha test or fog.
    // TextureMad (shaders/texmad_f.gxp, texel * diffuse + specular) stopped a real
    // Vita's GPU at its first draw (2026-10-13). Not packaged: a request for it
    // fails in fragment() instead of reaching the GPU.
    enum class Program : uint8_t { Combine, Uniform, Diffuse, TextureTint, TextureMad };
    Program program = Program::Combine;
    float tint[4] = {1, 1, 1, 1};
    bool repeatU = false, repeatV = false;
    bool linearMin = true, linearMag = true;
    int addressU = 2, addressV = 2; // Repeat=0, Mirror=1, Clamp=2
};
class Device {
public:
    struct Statistics {
        uint64_t draws = 0, scenes = 0, targetSwitches = 0, readbacks = 0;
        uint64_t uploadBytes = 0, gpuWaits = 0, frames = 0;
        uint64_t replacedUploads = 0;   // whole uploads to a texture in flight, given new storage
        uint64_t waitUs = 0, longestWaitUs = 0, fenceTimeouts = 0;   // CPU time spent waiting for the GPU
        uint64_t drawUs = 0, programUs = 0, copyUs = 0;   // CPU time in draw(): all, program lookup, vertex copy
    };
    Device();
    ~Device();
    bool initialize(const char *shaderDirectory = "app0:/shaders");
    const char *error() const;
    Statistics statistics() const;
    // nullptr target means the display. Targets/textures contain byte RGBA8.
    vita2d_texture *createTexture(uint32_t width, uint32_t height, bool renderTarget = false);
    bool upload(vita2d_texture *, const void *rgba, uint32_t sourcePitch,
                int x = 0, int y = 0, int width = -1, int height = -1);
    void destroyTexture(vita2d_texture *);
    DepthBuffer *createDepth(uint32_t width, uint32_t height, bool sixteenBit);
    void destroyDepth(DepthBuffer *);
    bool target(vita2d_texture *, DepthBuffer *depth = nullptr);
    // Pixel viewport is relative to the current target, with top-left origin.
    void viewport(float x, float y, float width, float height, float minDepth = 0, float maxDepth = 1);
    bool draw(const Vertex *, uint32_t count, vita2d_texture *, const DrawState &);
    // stage() copies vertices into this frame's GPU vertex memory once (nullptr when it is full);
    // drawStaged() draws from such a copy, so several passes over the same geometry copy it once.
    const Vertex *stage(const Vertex *, uint32_t count);
    // reserve() gives space for count vertices in the same memory, for the caller to fill
    // (then drawStaged); it waits for the GPU and rewinds when full, like draw(). nullptr on failure.
    Vertex *reserve(uint32_t count);
    bool drawStaged(const Vertex *staged, uint32_t count, vita2d_texture *, const DrawState &);
    // Clear obeys the requested rectangle, independent of preceding draw state.
    bool clear(bool color, bool depth, const float rgba[4], float depthValue,
               int x = 0, int y = 0, int width = -1, int height = -1);
    bool finish();
    bool present();
    bool read(vita2d_texture *, void *rgba, uint32_t destinationPitch);
    uint32_t targetWidth() const;
    uint32_t targetHeight() const;
private:
    struct Impl;
    Impl *impl;
};
}
