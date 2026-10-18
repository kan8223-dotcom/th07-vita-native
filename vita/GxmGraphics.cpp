// TH07 (Perfect Cherry Blossom) native Vita drawing: the game's ZunGraphics calls on the
// shared GXM Device (~/touhou-modern-vita/GxmRenderer.*), rewritten 2026-10-13 after the
// TH06 port's device findings (~/touhou-modern-vita/VITA_LESSONS_FROM_TH06.md).
//
// Only the three fragment programs a real Vita has run correctly are used:
//   DrawState::Program::Uniform     (not needed here)
//   DrawState::Program::Diffuse     the interpolated vertex colour
//   DrawState::Program::TextureTint texel * a uniform tint
// The general combine program (Program::Combine, shaders/draw_f.gxp, 849 instructions)
// drew noise and stopped the GPU on a real Vita, and so did texel*a+b (texmad_f): this file
// never requests either.
//
// What the GL port's one shader computes (src/graphics/Gles.cpp) is rebuilt from those:
//   textured:   colour = op(texel, arg) (TH07 issues MODULATE), arg = vertex colour,
//               texture factor or white (SetTextureArg)
//   untextured: colour = arg
//   then fog:   rgb = mix(fog colour, rgb, clamp(f)), f = (far - d) / (far - near)
// and the game's blend. A textured draw is cut into runs of triangles of one colour, each
// drawn as texel * tint with the game's blend. Fogged triangles, and triangles whose vertex
// alpha differs (the enemy trails), are drawn with several passes that keep a per-pixel
// value in the target's alpha channel (nothing in TH07 blends with the destination alpha;
// ReadPixels reports alpha as 255, like the original's X8R8G8B8 back buffer), as the TH06
// port does for its fog. A pass sequence covers a group of consecutive triangles that do
// not overlap on screen: the scratch alpha holds one value per pixel.
// Fog is linear in the interpolated distance and clamped per pixel in GL. Triangles that
// cross f = 0 or f = 1 are cut along those lines (the iso-lines of an affinely interpolated
// attribute are straight on screen), so each piece interpolates a clamped f exactly; the
// pieces with f = 1 are plain draws.
// The alpha test (GL discards texels with alpha < 4/255: they draw nothing and write no
// depth) has no fixed-function equivalent in GXM, and a discarding program would be a new
// shader. Its colour effect is below 4/255 of a texel; its depth effect is not: the stage
// backgrounds (stage 1 trees, stage 2) write depth from transparent texels and the layers
// behind them showed rectangular holes. So a textured draw that writes depth under the alpha
// test draws its colour without depth, then a depth-only pass (Diffuse program, colour mask
// 0, as the Device's depth clear) covers the texture cells whose every reachable texel
// passes the test (see alphaTestedDepth). Texels in a cell that mixes passing and failing
// texels write no depth.
#include "GxmRenderer.hpp"
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
#include "ModernGxmRenderer.hpp"
#include "MultipassColour.hpp"
#endif
#include "AnmManager.hpp"
#include "GameManager.hpp"
#include "Supervisor.hpp"
#include "Platform.hpp"
#ifdef TH07_VITA
#include "CoreRuntime.hpp"
#include "FrameWorker.hpp"
#include "BaryClip.hpp"
#include "ThresholdRunsCache.hpp"
#include "DepthRunWindow.hpp"
#include "VertexColourCache.hpp"
#include "PlainRunReorder.hpp"
#include "ScratchSpan.hpp"
#include "SpriteVertexReuse.hpp"
#include "SpriteQuadPacking.hpp"
#include "ColourBits.hpp"
#include "DepthFanReuse.hpp"
#include "SnapshotArena.hpp"
#include "ReusableFrameWorker.hpp"
#include "ScreenQuadCull.hpp"
#include "SimpleSpriteBatch.hpp"
#include "ScreenAxisCache.hpp"
#include "ViewportInside.hpp"
#include "SimpleScreenQuad.hpp"
#include "PowerTwoCellWrap.hpp"
#include <atomic>
#include <memory>
#include <malloc.h>
#endif
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
#include <arm_neon.h>
#endif

namespace {
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
using touhou::modernvita::Device;
using Vertex=touhou::modernvita::ProvenVertex;
#else
using touhou::vita::Device;
using touhou::vita::Vertex;
#endif
using touhou::vita::DrawState;
using Program = DrawState::Program;

constexpr unsigned kGameWidth = 640, kGameHeight = 480;
// A fog factor this close to 1 changes no 8-bit colour by more than rounding.
constexpr float kFogOne = 1.0f - 0.5f / 255.0f;
constexpr uint32_t kMaskRgb = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B;
constexpr uint32_t kMaskA = SCE_GXM_COLOR_MASK_A;

inline uint64_t nowUs() { return sceKernelGetProcessTimeWide(); }
// Only diagnostic sub-phase clocks may be omitted. Whole-draw/frame clocks,
// presentation, the startup calibration and Device's fence safety stay real.
inline uint64_t fineUs() {
#if defined(TH07_COARSE_GXM_TIMING) && TH07_COARSE_GXM_TIMING
    return 0;
#else
    return nowUs();
#endif
}

// D3DCOLOR (ZunColor.color): A in bits 24-31, R 16-23, G 8-15, B 0-7.
void colorFloats(uint32_t value, float *out)
{
    // Exact results of the previous divisions, not multiplication by an
    // approximate reciprocal. The table is immutable after its first use.
    static const std::array<float,256> normalized=[] {
        std::array<float,256> out{};
        for(unsigned i=0;i<out.size();++i)out[i]=i/255.0f;
        return out;
    }();
    out[0] = normalized[(value >> 16) & 255];
    out[1] = normalized[(value >> 8) & 255];
    out[2] = normalized[value & 255];
    out[3] = normalized[value >> 24];
}
bool sameRgba(const float *a, const float *b) {
#if defined(TH07_COLOUR_WORD_COMPARE) && TH07_COLOUR_WORD_COMPARE
    return th07_colourbits::equal<4>(a,b);
#else
    return std::memcmp(a,b,4*sizeof(float))==0;
#endif
}
bool sameRgb(const float *a, const float *b) {
#if defined(TH07_COLOUR_WORD_COMPARE) && TH07_COLOUR_WORD_COMPARE
    return th07_colourbits::equal<3>(a,b);
#else
    return std::memcmp(a,b,3*sizeof(float))==0;
#endif
}
bool powerOfTwo(uint32_t v) { return v && !(v & (v - 1)); }

// Separating-axis overlap test of two screen triangles (TH06 vita/GxmGfx.cpp, unchanged):
// touching along an edge or at a vertex is not an overlap. Normalized device units.
struct ScreenTriangle {
    float x[3], y[3], minX, maxX, minY, maxY;
    bool unbounded;
};
ScreenTriangle screenTriangle(const Vertex *v)
{
    ScreenTriangle t{};
    t.minX = t.minY = 1e30f; t.maxX = t.maxY = -1e30f;
    for (unsigned i = 0; i < 3; ++i)
    {
        const float w = v[i].position[3];
        if (!(w > 1e-6f)) { t.unbounded = true; return t; }   // crosses the eye plane
        t.x[i] = v[i].position[0] / w; t.y[i] = v[i].position[1] / w;
        t.minX = std::min(t.minX, t.x[i]); t.maxX = std::max(t.maxX, t.x[i]);
        t.minY = std::min(t.minY, t.y[i]); t.maxY = std::max(t.maxY, t.y[i]);
    }
    return t;
}
bool overlaps(const ScreenTriangle &a, const ScreenTriangle &b)
{
    if (a.unbounded || b.unbounded) return true;
    constexpr float slack = 1e-4f;
    if (a.maxX <= b.minX + slack || b.maxX <= a.minX + slack || a.maxY <= b.minY + slack || b.maxY <= a.minY + slack)
        return false;
    const ScreenTriangle *both[2] = {&a, &b};
    for (const ScreenTriangle *t : both)
        for (unsigned e = 0; e < 3; ++e)
        {
            const float nx = -(t->y[(e + 1) % 3] - t->y[e]), ny = t->x[(e + 1) % 3] - t->x[e];
            const float length2 = nx * nx + ny * ny;
            if (length2 < 1e-18f) continue;
            float minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
            for (unsigned i = 0; i < 3; ++i)
            {
                const float pa = a.x[i] * nx + a.y[i] * ny, pb = b.x[i] * nx + b.y[i] * ny;
                minA = std::min(minA, pa); maxA = std::max(maxA, pa);
                minB = std::min(minB, pb); maxB = std::max(maxB, pb);
            }
            const float limit2 = slack * slack * length2;
            const float ab = maxA - minB, ba = maxB - minA;
            if (ab <= 0 || ab * ab <= limit2 || ba <= 0 || ba * ba <= limit2) return false;
        }
    return true;
}

// Every attribute of a vertex is affine in object space (clip coordinates included), so a
// point on an edge interpolates all of them with one parameter. The edge is always walked
// from the same end (the smaller position), so the two triangles sharing it make the same
// new vertex bit for bit: no cracks.
Vertex edgePoint(const Vertex &p, const Vertex &q, float threshold)
{
    const bool swap = std::memcmp(p.position, q.position, sizeof(p.position)) > 0;
    const Vertex &a = swap ? q : p, &b = swap ? p : q;
    const float t = (threshold - a.fog[3]) / (b.fog[3] - a.fog[3]);
    Vertex out;
    const float *fa = reinterpret_cast<const float *>(&a), *fb = reinterpret_cast<const float *>(&b);
    float *fo = reinterpret_cast<float *>(&out);
    for (unsigned i = 0; i < sizeof(Vertex) / sizeof(float); ++i) fo[i] = fa[i] + (fb[i] - fa[i]) * t;
    out.fog[3] = threshold;
    return out;
}
// Sutherland-Hodgman: the part of a convex polygon with sign * (f - threshold) >= 0.
void clipPolygon(const std::vector<Vertex> &in, float threshold, float sign, std::vector<Vertex> &out)
{
    out.clear();
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i)
    {
        const Vertex &p = in[i], &q = in[(i + 1) % n];
        const float dp = sign * (p.fog[3] - threshold), dq = sign * (q.fog[3] - threshold);
        if (dp >= 0) out.push_back(p);
        if ((dp > 0 && dq < 0) || (dp < 0 && dq > 0)) out.push_back(edgePoint(p, q, threshold));
    }
}

class VitaGraphics final : public ZunGraphics {
    Device device;
    vita2d_texture *frame = nullptr;
    struct Texture {
        vita2d_texture *image = nullptr;
        uint32_t width = 0, height = 0;
        bool used = false;
        std::vector<uint8_t> shadow;   // RGBA copy for sub-image updates (made on the first one)
        bool zero = false;             // the last whole upload was all zero (text, capture atlases)
        // The alpha test's view of the texture: per cell of kCell x kCell texels, the smallest
        // alpha the bilinear filter returns anywhere inside the cell (wrapped like the sampler).
        std::vector<uint8_t> cellAlpha;
        uint32_t cellsX = 0, cellsY = 0, version = 0;   // version: unique per content (cut cache key)
        uint8_t minAlpha = 0;
        std::array<uint16_t,256> nextCellAlpha{}; // canonical >= threshold: identical passing-cell set
        mutable std::vector<uint32_t> opaquePrefix;
        mutable uint32_t opaquePrefixVersion = 0;
        struct DepthRuns {
            std::vector<uint32_t> prefix;
            std::vector<std::vector<std::pair<int,int>>> rows;
            uint32_t version = 0, threshold = 0;
            bool rowsOnly=false;
            uint32_t rowWidth=0;
        };
        // Keep the usual reference-4 cache while a fading sprite/background
        // uses another reference. Both have bounded storage, not 256 atlases.
        mutable DepthRuns depthRuns[2];
    };
    static uint32_t nextVersion() { static uint32_t counter; return ++counter; }
#ifndef TH07_VITA_DEPTH_CELL
#define TH07_VITA_DEPTH_CELL 2   // texels; 4 drew ~2x the stage 1 mismatch of 2 (PC check), 1 ~1.3x the geometry
#endif
    static constexpr uint32_t kCell = TH07_VITA_DEPTH_CELL;
    static void buildCells(Texture &t, const uint8_t *rgba, uint32_t pitch, uint32_t firstRow = 0, uint32_t endRow = ~0u)
    {
        const bool wrap = powerOfTwo(t.width) && powerOfTwo(t.height);
        const int w = int(t.width), h = int(t.height);
        if (t.cellAlpha.empty() || (wrap && (firstRow == 0 || endRow >= t.height)))   // wrapped footprints: all
        {
            t.cellsX = (t.width + kCell - 1) / kCell; t.cellsY = (t.height + kCell - 1) / kCell;
            if (t.cellAlpha.empty()) t.cellAlpha.assign(size_t(t.cellsX) * t.cellsY, 0);
            firstRow = 0; endRow = t.height;
        }
        // Cell rows whose footprint touches texel rows [firstRow, endRow).
        const int cy0 = std::max(0, (int(firstRow) - 1) / int(kCell)), cy1 = std::min(int(t.cellsY) - 1, int(std::min(endRow, t.height)) / int(kCell));
        const auto at = [&](int x, int y) {
            x = wrap ? (x & (w - 1)) : std::min(w - 1, std::max(0, x));
            y = wrap ? (y & (h - 1)) : std::min(h - 1, std::max(0, y));
            return int(rgba[size_t(y) * pitch + size_t(x) * 4 + 3]);
        };
        // The bilinear filter's alpha at a point given in half texels (odd = a texel centre,
        // even = the boundary between two texels), times 4. Over a cell it is smallest at one of
        // these points: the texel centres inside, the boundary points between them and the
        // cell's edges, and its corners (the filter is bilinear between texel centres).
        const auto filtered4 = [&](int x2, int y2) {
            const int xa = (x2 - 1) >> 1, xb = x2 >> 1, ya = (y2 - 1) >> 1, yb = y2 >> 1;   // equal at centres
            return at(xa, ya) + at(xb, ya) + at(xa, yb) + at(xb, yb);
        };
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = 0; cx < int(t.cellsX); ++cx)
            {
                int lo = 4 * 255;
                for (int y2 = 2 * cy * int(kCell); y2 <= 2 * (cy + 1) * int(kCell) && lo; ++y2)
                    for (int x2 = 2 * cx * int(kCell); x2 <= 2 * (cx + 1) * int(kCell); ++x2)
                        lo = std::min(lo, filtered4(x2, y2));
                t.cellAlpha[size_t(cy) * t.cellsX + cx] = uint8_t(lo / 4);   // rounded down: never optimistic
            }
        bool present[256]{};t.minAlpha=255;
        for(uint8_t alpha:t.cellAlpha){present[alpha]=true;t.minAlpha=std::min(t.minAlpha,alpha);}
        uint16_t next=256;
        for(int alpha=255;alpha>=0;--alpha){if(present[alpha])next=uint16_t(alpha);t.nextCellAlpha[alpha]=next;}
        t.version = nextVersion();
    }
    std::vector<Texture> textures{1};   // index = GfxTextureHandle id, 0 = none
    uint32_t bound = 0;
    ZunMatrix matrices[4];
    ZunMatrix cachedModelview, cachedMvp;
    bool transformsDirty = true;
    ZunViewport view = {0, 0, 640, 480, 0, 1};
    bool blendEnabled = true, depthTest = true, alphaTest = true, fogEnabled = false, failed = false;
    bool depthWrite = true;
    DepthFunc depthFunc = DEPTH_FUNC_ALWAYS;
    BlendMode srcBlend = BLEND_ALPHA, dstBlend = BLEND_ALPHA;
    uint32_t textureFactor = 0xffffffff, fogColor = 0, clearColor = 0xff000000;
    TextureArg textureArg = TEX_ARG_DIFFUSE;
    ColorOp rgbOp = COLOR_OP_MODULATE, alphaOp = COLOR_OP_MODULATE;
    float fogNear = 0, fogFar = 1, clearDepth = 1;
    uint8_t alphaRef = 4;
    std::vector<uint8_t> conversion, readback;
    std::vector<Vertex> source, expanded, visible, cut, aux[4], lit;
#if defined(TH07_PLAIN_RUN_REORDER) && TH07_PLAIN_RUN_REORDER
    th07_plainruns::Plan plainOrder;
    std::vector<Vertex> plainOrdered;
    uint64_t plainMoved=0;
#endif
    bool sourceIsQuadTriangles=false;
    bool sourceAllSimple=false;
    std::vector<Vertex> polygon[3];
    struct TriangleInfo { uint8_t kind; };
    std::vector<TriangleInfo> info;

    // FoggedOpaque: fogged, texel alpha 255 under the whole triangle and tint alpha 1, with a
    // blend that then keeps nothing of the target (opaqueFogPasses).
    enum Kind : uint8_t { Plain, Fogged, AlphaVaries, RgbVaries, FoggedOpaque };

    void check(bool ok)
    {
        if (!ok && !failed)
        {
            failed = true;
            std::printf("[TH07 GXM] %s\n", device.error());
            th07_vita_log("GXM ERROR: %s", device.error());
            th07_vita_stop(device.error());
        }
    }
    void flushSprites() {
#ifndef TH07_VITA
        if (g_AnmManager && g_AnmManager->spritesToDraw) g_AnmManager->Flush();
#endif
    } // Native producer flushes engine-owned sprites before queuing state changes.
    // GL (Gles.cpp) takes no depth range from the viewport (TH07 always gives 0..1; a stage's
    // local viewport leaves it uninitialized).
    void applyViewport() { device.viewport(view.x, view.y, view.width, view.height, 0, 1); }
    Texture *boundTexture()
    {
        return bound && bound < textures.size() && textures[bound].used ? &textures[bound] : nullptr;
    }

    // ---- statistics (logged per slow frame and every 600 frames) ----
    unsigned presentedFrames = 0;
    Device::Statistics lastFrameStatistics{}, windowStatistics{};
    uint64_t lastPresentUs = 0, drawUsThisFrame = 0, drawUsWindow = 0, drawUsWorst = 0;
    struct Costs {
        uint64_t convertUs = 0, prepareUs = 0, vertices = 0, plainRuns = 0, groups = 0, passes = 0;
        uint64_t depthCutUs = 0, depthPrepareUs = 0, scheduleUs = 0, depthThreshold4 = 0, depthThresholdOther = 0, depthCulled = 0, colorCulled = 0;
        uint64_t depthRunsBuildUs=0,depthRunsBuilds=0;
        uint64_t transformUs=0,visibilityUs=0,fogCutUs=0;
        uint64_t matrixRebuilds=0,sourceScreenVertices=0,sourceWorldVertices=0;
        uint64_t duplicateScreenVertices=0;
        uint64_t alphaCellsUs=0,textureUploadUs=0;
        uint64_t cutTriangles = 0, alphaGradientTriangles = 0, rgbGradientTriangles = 0;
        uint64_t unboundTextureDraws = 0, unsupportedOps = 0, subImageUploads = 0, imageUploads = 0;
        uint64_t depthVertices = 0, depthCellTriangles = 0, depthUnmapped = 0, gameDraws = 0, batches = 0;
        uint64_t scheduledBatches = 0, levels = 0, opaqueFogGroups = 0;
        uint64_t displayCpuUs=0,presentCpuUs=0,targetCpuUs=0;
        uint64_t depthRows=0,depthInputRuns=0,depthMergedRuns=0,depthRectangles=0,depthEmptyRectangles=0,depthClipPoints=0;
        uint64_t depthFanReused=0;
        uint64_t earlyScreenQuads=0;
        uint64_t multipassInputVertices=0,multipassUsedVertices=0;
        uint64_t simpleNativeVertices=0,simpleNativeBatches=0;
        uint64_t screenAxisHits=0,screenAxisMisses=0;
        uint64_t compactUvVertices=0;
    } costs, frameCosts;
    void noteFrame(uint64_t presentStartUs)
    {
        const uint64_t now = nowUs();
        const auto statistics = device.statistics();
        if (lastPresentUs)
        {
            const uint64_t frameUs = now - lastPresentUs;
            const uint64_t waitUs = statistics.waitUs - lastFrameStatistics.waitUs;
            if (frameUs > 25000 || waitUs > 8000)
                th07_vita_log("GXM slow frame=%u ms=%.1f draw_cpu_ms=%.1f present_ms=%.1f wait_ms=%.1f waits=%llu "
                              "timeouts=%llu draws=%llu upload_kb=%llu replaced=%llu passes=%llu groups=%llu state=%d stage=%d "
                              "convert_ms=%.1f prepare_ms=%.1f device_draw_ms=%.1f copy_ms=%.1f depth_vertices=%llu depth_cell_tris=%llu "
                              "depth_cut_ms=%.1f depth_prepare_ms=%.1f schedule_ms=%.1f depth_ref4=%llu depth_ref_other=%llu depth_culled=%llu color_culled=%llu "
                              "transform_ms=%.1f visibility_ms=%.1f fog_cut_ms=%.1f runs_build_ms=%.1f runs_rebuilds=%llu",
                    presentedFrames, frameUs / 1000.0, drawUsThisFrame / 1000.0, (now - presentStartUs) / 1000.0,
                    waitUs / 1000.0, (unsigned long long)(statistics.gpuWaits - lastFrameStatistics.gpuWaits),
                    (unsigned long long)(statistics.fenceTimeouts - lastFrameStatistics.fenceTimeouts),
                    (unsigned long long)(statistics.draws - lastFrameStatistics.draws),
                    (unsigned long long)((statistics.uploadBytes - lastFrameStatistics.uploadBytes) / 1024),
                    (unsigned long long)(statistics.replacedUploads - lastFrameStatistics.replacedUploads),
                    (unsigned long long)(costs.passes - frameCosts.passes),
                    (unsigned long long)(costs.groups - frameCosts.groups), drawnState,
                    drawnStage,
                    (costs.convertUs-frameCosts.convertUs)/1000.0,(costs.prepareUs-frameCosts.prepareUs)/1000.0,
                    (statistics.drawUs-lastFrameStatistics.drawUs)/1000.0,(statistics.copyUs-lastFrameStatistics.copyUs)/1000.0,
                    (unsigned long long)(costs.depthVertices-frameCosts.depthVertices),
                    (unsigned long long)(costs.depthCellTriangles-frameCosts.depthCellTriangles),
                    (costs.depthCutUs-frameCosts.depthCutUs)/1000.0,
                    (costs.depthPrepareUs-frameCosts.depthPrepareUs)/1000.0,
                    (costs.scheduleUs-frameCosts.scheduleUs)/1000.0,
                    (unsigned long long)(costs.depthThreshold4-frameCosts.depthThreshold4),
                    (unsigned long long)(costs.depthThresholdOther-frameCosts.depthThresholdOther),
                    (unsigned long long)(costs.depthCulled-frameCosts.depthCulled),
                    (unsigned long long)(costs.colorCulled-frameCosts.colorCulled),
                    (costs.transformUs-frameCosts.transformUs)/1000.0,
                    (costs.visibilityUs-frameCosts.visibilityUs)/1000.0,
                    (costs.fogCutUs-frameCosts.fogCutUs)/1000.0,
                    (costs.depthRunsBuildUs-frameCosts.depthRunsBuildUs)/1000.0,
                    costs.depthRunsBuilds-frameCosts.depthRunsBuilds);
        }
        lastFrameStatistics = statistics; lastPresentUs = now; frameCosts = costs;
        drawUsWindow += drawUsThisFrame; drawUsWorst = std::max(drawUsWorst, drawUsThisFrame); drawUsThisFrame = 0;
        if (presentedFrames % 600) return;
#if defined(TH07_PLAIN_RUN_REORDER) && TH07_PLAIN_RUN_REORDER
        plainMoved=0;
#endif
#if defined(TH07_THRESHOLD_CACHE) && TH07_THRESHOLD_CACHE
        thresholdHits=thresholdMisses=0;
#endif
        windowStatistics = statistics; costs = Costs{}; frameCosts = Costs{};
        drawUsWindow = 0; drawUsWorst = 0;
    }

    int drawnState = -1, drawnStage = -1, drawnStageFrame = -1;
    // What the next frame's draw chain will draw: GameWindow::Render draws, then runs the
    // calc chain, then presents; so the state after one present is the state drawn next.
    void sampleDrawnState()
    {
        drawnState = g_Supervisor.curState;
        drawnStage = g_GameManager.currentStage;
        drawnStageFrame = drawnState == 2 ? g_GameManager.framesThisStage : -1;
    }

public:
    bool hasError()const{return failed;}
    bool fitOutput=false;
    void outputFit(bool value){fitOutput=value;}
    void frameInfo(int state,int stage,int frame,bool fit){drawnState=state;drawnStage=stage;drawnStageFrame=frame;fitOutput=fit;}
    bool initialize()
    {
        th07_vita_log("GXM init begin; programs uniform/diffuse/tint only");
#if defined(TH07_BARY_INTERPOLATE_NEON) && TH07_BARY_INTERPOLATE_NEON
        const bool interpolationExact=th07_baryinterpolation::selfTest(10000);
        th07_vita_log("GXM bary interpolation self-test cases=10000 byte_exact=%d core=%d",int(interpolationExact),sceKernelGetCpuId());
        if(!interpolationExact){failed=true;th07_vita_stop("bary interpolation differs from scalar reference");return false;}
#endif
        // Without the proven programs the Device would fall back to the combine program
        // (its clear does): refuse to start instead.
        for (const char *name : {"app0:/shaders/proven_uniform_f.gxp", "app0:/shaders/proven_diffuse_f.gxp",
                                 "app0:/shaders/proven_tint_f.gxp"})
        {
            FILE *probe = std::fopen(name, "rb");
            if (!probe)
            {
                th07_vita_log("GXM ERROR: %s missing; not drawing with the combine program", name);
                th07_vita_stop("proven fragment program missing");
                failed = true;
                return false;
            }
            std::fclose(probe);
        }
        if (!device.initialize()) { check(false); return false; }
        for (auto &m : matrices) m.Identity();
        frame = device.createTexture(kGameWidth, kGameHeight, true);
        if (!frame) { check(false); return false; }
        check(device.target(frame)); applyViewport();
        const float black[4] = {0, 0, 0, 1};
        check(device.clear(true, true, black, 1));
        th07_vita_log("GXM framebuffer ready=%u", unsigned(!failed));
        return !failed;
    }
    ~VitaGraphics() override { Exit(); }
    void Exit() override
    {
        pending.clear();
        for (auto &texture : textures)
            if (texture.image) { device.destroyTexture(texture.image); texture.image = nullptr; }
        textures.assign(1, Texture{});
        if (frame) { device.destroyTexture(frame); frame = nullptr; }
    }
    RendererType GetType() override { return RENDERER_GXM; }
    void SetFogRange(float nearPlane, float farPlane) override { fogNear = nearPlane; fogFar = farPlane; }
    void SetFogColor(ZunColor color) override { fogColor = color.color; }
    void SetColorOp(TextureOpComponent component, ColorOp op) override
    {
        if (component == COMPONENT_RGB) rgbOp = op; else alphaOp = op;
    }
    void SetTextureFactor(ZunColor color) override { textureFactor = color.color; }
    void SetTextureArg(TextureArg arg) override { textureArg = arg; }
    void SetTransformMatrix(TransformMatrix type, const ZunMatrix &m) override {
        if(std::memcmp(&matrices[type],&m,sizeof(m))==0)return;
        matrices[type]=m;
        if(type!=MATRIX_TEXTURE)transformsDirty=true;
    }
    void SetTextureFilter() override {}   // GL_LINEAR + GL_REPEAT, like Gles.cpp: every draw samples so
    void GetViewport(ZunViewport &value) override { value = view; }
    void SetViewport(const ZunViewport &value) override
    {
        if (std::memcmp(&value, &view, sizeof(view)) != 0) beforeStateChange();
        view = value; applyViewport();
    }
    void Enable(Capabilities cap) override { capability(cap, true); }
    void Disable(Capabilities cap) override { capability(cap, false); }
    void capability(Capabilities cap, bool value)
    {
        // Fog is in the converted vertices; the others are read when a batch is drawn.
        if (cap == CAPS_BLEND) { if (blendEnabled != value) beforeStateChange(); blendEnabled = value; }
        else if (cap == CAPS_DEPTH_TEST) { if (depthTest != value) beforeStateChange(); depthTest = value; }
        else if (cap == CAPS_ALPHA_TEST) { if (alphaTest != value) beforeStateChange(); alphaTest = value; }
        else if (cap == CAPS_FOG) fogEnabled = value;
    }
    void SetBlendMode(BlendMode src, BlendMode dst) override
    {
        flushSprites();
        if (src != srcBlend || dst != dstBlend) beforeStateChange();
        srcBlend = src; dstBlend = dst;
    }
    void SetDepthMask(bool enabled) override
    {
        flushSprites();
        if (enabled != depthWrite) beforeStateChange();
        depthWrite = enabled;
    }
    void SetDepthFunc(DepthFunc value) override
    {
        flushSprites();
        if (value != depthFunc) beforeStateChange();
        depthFunc = value;
    }
    void SetClearDepth(float value) override { clearDepth = value; }
    void SetClearColor(ZunColor color) override { clearColor = color.color; }
    void SetAlphaTestRef(uint8_t value) override
    {
        if (value != alphaRef) beforeStateChange();
        alphaRef = value;
    }
    void Clear(uint32_t bits) override
    {
        // glClear (Gles.cpp) clears the whole target whatever the viewport is.
        beforeStateChange();
        if (failed) return;
        float rgba[4]; colorFloats(clearColor, rgba);
        check(device.clear(bits & CLEAR_COLOR_BUFFER, bits & CLEAR_DEPTH_BUFFER, rgba, clearDepth));
    }
    GfxTextureHandle CreateTexture() override
    {
        for (uint32_t id = 1; id < textures.size(); ++id)
            if (!textures[id].used) { textures[id] = Texture{}; textures[id].used = true; return {id}; }
        textures.emplace_back(); textures.back().used = true;
        return {uint32_t(textures.size() - 1)};
    }
    void BindTexture(GfxTextureHandle handle) override
    {
        if (handle.id != bound) beforeStateChange();
        bound = handle.id;
    }
    void DeleteTexture(GfxTextureHandle handle) override
    {
        beforeStateChange();
        if (!handle.id || handle.id >= textures.size() || !textures[handle.id].used) return;
        auto &texture = textures[handle.id];
        if (texture.image) device.destroyTexture(texture.image);
        texture = Texture{};
        if (bound == handle.id) bound = 0;
    }
    void SetTextureImage(uint32_t width, uint32_t height, PixelFormat format, PixelDataType type, const void *data) override
    {
        beforeStateChange();   // the batch samples the texture as it was
        Texture *texture = boundTexture();
        if (failed || !texture || !width || !height) return;
        const bool fresh = !texture->image || texture->width != width || texture->height != height;
        if (fresh)
        {
            if (texture->image) device.destroyTexture(texture->image);
            texture->image = device.createTexture(width, height);   // reads as zero
            texture->width = width; texture->height = height;
            if (!texture->image) { check(false); return; }
        }
        texture->shadow.clear();
        texture->cellAlpha.clear();
        ++costs.imageUploads;
        conversion.resize(size_t(width) * height * 4);
        if (!data) std::fill(conversion.begin(), conversion.end(), 0);
        else convertPixels(width, height, format, type, data, conversion.data());
        buildCells(*texture, conversion.data(), width * 4);
        texture->zero = !data || std::all_of(conversion.begin(), conversion.end(), [](uint8_t b) { return b == 0; });
        if (!data && fresh) return;
        check(device.upload(texture->image, conversion.data(), width * 4));
    }
    static void convertPixels(uint32_t width, uint32_t height, PixelFormat format, PixelDataType type, const void *data,
                              uint8_t *rgba)
    {
        const auto *source = static_cast<const uint8_t *>(data);
        const size_t pixels = size_t(width) * height;
        if (type == PIXEL_UNSIGNED_BYTE && format == PIXEL_RGBA) { std::memcpy(rgba, source, pixels * 4); return; }
        const unsigned step = type == PIXEL_UNSIGNED_BYTE ? (format == PIXEL_RGB ? 3 : 4) : 2;
        for (size_t p = 0; p < pixels; ++p, source += step)
        {
            uint8_t *pixel = rgba + p * 4;
            if (type == PIXEL_UNSIGNED_BYTE)
            {
                pixel[0] = source[format == PIXEL_BGRA ? 2 : 0]; pixel[1] = source[1];
                pixel[2] = source[format == PIXEL_BGRA ? 0 : 2]; pixel[3] = format == PIXEL_RGB ? 255 : source[3];
                continue;
            }
            uint16_t value; std::memcpy(&value, source, sizeof(value));
            // Unsigned normalized to 8 bits rounds to nearest (GL's conversion).
            const auto widen = [](unsigned v, unsigned bits) { const unsigned max = (1u << bits) - 1; return uint8_t((v * 255 + max / 2) / max); };
            if (type == PIXEL_UNSIGNED_SHORT_5_6_5)
            { pixel[0] = widen(value >> 11, 5); pixel[1] = widen((value >> 5) & 63, 6); pixel[2] = widen(value & 31, 5); pixel[3] = 255; }
            else if (type == PIXEL_UNSIGNED_SHORT_5_5_5_1)
            { pixel[0] = widen(value >> 11, 5); pixel[1] = widen((value >> 6) & 31, 5); pixel[2] = widen((value >> 1) & 31, 5); pixel[3] = (value & 1) * 255; }
            else
            { pixel[0] = (value >> 12) * 17; pixel[1] = ((value >> 8) & 15) * 17; pixel[2] = ((value >> 4) & 15) * 17; pixel[3] = (value & 15) * 17; }
        }
    }
    void SetTextureSubImage(int x, int y, int width, int height, const void *data) override
    {
        // RGBA bytes (glTexSubImage2D GL_RGBA/GL_UNSIGNED_BYTE in Gles.cpp): text rows, the pause
        // capture. A partial upload into a texture the GPU may still read waits for it; the whole
        // texture (kept here as a copy) goes to fresh storage instead (Device::upload).
        beforeStateChange();
        Texture *texture = boundTexture();
        if (failed || !texture || !texture->image || !data || width <= 0 || height <= 0 || x < 0 || y < 0 ||
            uint32_t(x + width) > texture->width || uint32_t(y + height) > texture->height)
            return;
        ++costs.subImageUploads;
        const uint32_t pitch = texture->width * 4;
        if (texture->shadow.empty() && texture->zero)
            texture->shadow.assign(size_t(pitch) * texture->height, 0);   // no read back from GPU memory
        else if (texture->shadow.empty())
        {
            texture->shadow.resize(size_t(pitch) * texture->height);
            const auto *pixels = static_cast<const uint8_t *>(vita2d_texture_get_datap(texture->image));
            const uint32_t stride = vita2d_texture_get_stride(texture->image);
            for (uint32_t row = 0; row < texture->height; ++row)
                std::memcpy(texture->shadow.data() + size_t(row) * pitch, pixels + size_t(row) * stride, pitch);
        }
        for (int row = 0; row < height; ++row)
            std::memcpy(texture->shadow.data() + size_t(y + row) * pitch + size_t(x) * 4,
                        static_cast<const uint8_t *>(data) + size_t(row) * width * 4, size_t(width) * 4);
        buildCells(*texture, texture->shadow.data(), pitch, uint32_t(y), uint32_t(y + height));
        check(device.upload(texture->image, texture->shadow.data(), pitch));
    }
    void ReadPixels(int x, int y, int width, int height, void *data) override
    {
        // Top row first (Gles.cpp flips glReadPixels). Alpha is 255: the original's back
        // buffer has none, and here the target's alpha is the passes' scratch value.
        if (!data || width <= 0 || height <= 0) return;
        beforeStateChange();
        std::memset(data, 0, size_t(width) * height * 4);
        if (failed) return;
        readback.resize(kGameWidth * kGameHeight * 4);
        if (!device.read(frame, readback.data(), kGameWidth * 4)) { check(false); return; }
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width; ++col)
                if (x + col >= 0 && x + col < int(kGameWidth) && y + row >= 0 && y + row < int(kGameHeight))
                {
                    uint8_t *out = static_cast<uint8_t *>(data) + (size_t(row) * width + col) * 4;
                    std::memcpy(out, readback.data() + (size_t(y + row) * kGameWidth + x + col) * 4, 3);
                    out[3] = 255;
                }
    }
    void DrawPrimitiveUP(PrimitiveType type, int primitiveCount, const void *data, int stride) override
    {
        const uint64_t start = nowUs();
        insideDraw = true;
        drawInner(type, primitiveCount, data, stride);
        insideDraw = false;
        drawUsThisFrame += nowUs() - start;
    }
    void DrawPrimitive(PrimitiveType type, int first, int primitiveCount) override
    {
        // The unit quad of Gles.cpp's unitQuadVao: no colour attribute (white).
        const int count = type == PRIM_TRIANGLES ? primitiveCount * 3 : primitiveCount + 2;
        if (first < 0 || count <= 0 || first + count > 4) return;
        VertexTex1DiffuseXyz quad[4];
        const float xyuv[4][4] = {{-128, -128, 0, 0}, {128, -128, 1, 0}, {-128, 128, 0, 1}, {128, 128, 1, 1}};
        for (unsigned j = 0; j < 4; ++j)
        {
            quad[j].position = {xyuv[j][0], xyuv[j][1], 0};
            quad[j].diffuse.color = 0xffffffff;
            quad[j].textureUV = {xyuv[j][2], xyuv[j][3]};
        }
        DrawPrimitiveUP(type, primitiveCount, quad + first, sizeof(quad[0]));
    }

private:
    DrawState baseState() const
    {
        DrawState state;
        state.primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
        state.depthFunc = depthTest && depthFunc == DEPTH_FUNC_LEQUAL ? SCE_GXM_DEPTH_FUNC_LESS_EQUAL : SCE_GXM_DEPTH_FUNC_ALWAYS;
        state.depthWrite = depthTest && depthWrite;   // GL writes no depth without the depth test
        const auto source = srcBlend == BLEND_ALPHA ? SCE_GXM_BLEND_FACTOR_SRC_ALPHA : SCE_GXM_BLEND_FACTOR_ONE;
        const auto destination = dstBlend == BLEND_ALPHA ? SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                 : dstBlend == BLEND_ONE ? SCE_GXM_BLEND_FACTOR_ONE : SCE_GXM_BLEND_FACTOR_ZERO;
        state.blend = blendEnabled && !(source == SCE_GXM_BLEND_FACTOR_ONE && destination == SCE_GXM_BLEND_FACTOR_ZERO);
        state.colorBlend = state.alphaBlend = SCE_GXM_BLEND_FUNC_ADD;
        state.colorSrc = state.alphaSrc = source;
        state.colorDst = state.alphaDst = destination;
        state.linear = true;
        state.program = Program::TextureTint;
        return state;
    }
    // Source vertices -> `source`: clip position, uv, colour argument (the per-vertex tint)
    // and fog (rgb = fog colour, w = unclamped factor; 1 without fog).
    template<bool WordAligned,bool Screen>
    __attribute__((noinline)) bool convertImpl(PrimitiveType type, int primitiveCount, const void *data, int stride, bool textured,
                 unsigned &count)
    {
        constexpr bool screen=Screen;
        sourceAllSimple=false;
        count = type == PRIM_TRIANGLES ? primitiveCount * 3 : type == PRIM_QUADS ? primitiveCount * 4 : primitiveCount + 2;
        (screen?costs.sourceScreenVertices:costs.sourceWorldVertices)+=count;
        const bool directQuads=screen&&type==PRIM_QUADS;
        sourceIsQuadTriangles=directQuads;
        // Every attribute is overwritten below. Keep the high-water size so
        // alternating small/large sprite draws do not zero the large array again.
        if(directQuads){if(expanded.size()<count/4*6)expanded.resize(count/4*6);}
        else if(source.size()<count)source.resize(count);
        if(!screen&&transformsDirty){
            ++costs.matrixRebuilds;
            cachedModelview=matrices[MATRIX_MODEL]*matrices[MATRIX_VIEW];
            cachedMvp=cachedModelview*matrices[MATRIX_PROJECTION];
            transformsDirty=false;
        }
        const float (&M)[4][4] = cachedMvp.m;
        const float (&V)[4][4] = cachedModelview.m;
        const float (&T)[4][4] = matrices[MATRIX_TEXTURE].m;
        float fog[4]; colorFloats(fogColor, fog);
        const float fogExtent = fogFar - fogNear;
        const bool distanceFog=fogEnabled&&fogExtent!=0;
#if defined(TH07_SIMPLE_NATIVE_BATCHES) && TH07_SIMPLE_NATIVE_BATCHES
        if constexpr(screen)sourceAllSimple=directQuads&&!distanceFog&&th07_simplebatch::uniform(data,stride,count);
#endif
        float factor[4]; colorFloats(textureFactor, factor);
        const float white[4] = {1, 1, 1, 1};
        // The texture combine for each component: MODULATE takes arg, REPLACE the texel alone.
        const bool rgbArg = !textured || rgbOp != COLOR_OP_REPLACE, alphaArg = !textured || alphaOp != COLOR_OP_REPLACE;
        const float viewWidth = float(std::max(1u, view.width)), viewHeight = float(std::max(1u, view.height));
        const float viewX=float(view.x),viewY=float(view.y),drawFogFar=fogFar;
#if defined(TH07_SCREEN_AXIS_CACHE) && TH07_SCREEN_AXIS_CACHE
        th07_screenaxis::Cache xAxis,yAxis;
#endif
        const auto drawTextureArg=textureArg;
        Vertex* output=directQuads?expanded.data():source.data();
#if defined(TH07_VERTEX_COLOUR_CACHE) && TH07_VERTEX_COLOUR_CACHE
        th07_vertexcolour::Cache vertexColour;
        float identityFog[4];std::memcpy(identityFog,fog,12);identityFog[3]=1;
#endif
#if defined(TH07_SIMPLE_SCREEN_CONVERT) && TH07_SIMPLE_SCREEN_CONVERT
        if constexpr(screen)if(sourceAllSimple){
            float screenFog[4];std::memcpy(screenFog,fog,12);screenFog[3]=1;
            const float* constantArg=drawTextureArg==TEX_ARG_TFACTOR?factor:drawTextureArg==TEX_ARG_TEXTURE?white:nullptr;
            th07_simplescreen::convert(expanded.data(),data,count,stride,textured,
                viewX,viewY,viewWidth,viewHeight,constantArg,rgbArg,alphaArg,screenFog,colorFloats);
            return true;
        }
#endif
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
        float32x4_t m0{},m1{},m2{},m3{},v0{},v1{},v2{},v3{};
        if(!screen){m0=vld1q_f32(M[0]);m1=vld1q_f32(M[1]);m2=vld1q_f32(M[2]);m3=vld1q_f32(M[3]);}
        if(!screen&&distanceFog){v0=vld1q_f32(V[0]);v1=vld1q_f32(V[1]);v2=vld1q_f32(V[2]);v3=vld1q_f32(V[3]);}
#endif
        // Stage backgrounds submit one four-vertex world quad at a time,
        // including many wholly behind/outside the camera. Its attributes,
        // texture transform and four range-fog square roots are irrelevant
        // when a conservative homogeneous plane rejects the entire draw.
        const bool positionPrepared=!screen&&count==4;
        if(positionPrepared){
            for(unsigned i=0;i<count;++i){
                const auto* src=static_cast<const uint8_t*>(data)+size_t(i)*stride;
                if constexpr(WordAligned)src=static_cast<const uint8_t*>(__builtin_assume_aligned(src,4));
                float p[3];std::memcpy(p,src,12);const float x=p[0],y=p[1],z=p[2];
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
                const auto xy=vaddq_f32(vmulq_n_f32(m0,x),vmulq_n_f32(m1,y));
                vst1q_f32(source[i].position,vaddq_f32(vaddq_f32(xy,vmulq_n_f32(m2,z)),m3));
#else
                for(int j=0;j<4;++j)source[i].position[j]=M[0][j]*x+M[1][j]*y+M[2][j]*z+M[3][j];
#endif
            }
            bool anyInside=false;
            for(unsigned i=0;i<count;++i){const float* p=source[i].position;
                anyInside|=p[3]>0&&std::fabs(p[0])<=p[3]&&std::fabs(p[1])<=p[3];}
            if(!anyInside)for(int plane=0;plane<5;++plane){
                float d[4],magnitude=1;
                for(int k=0;k<4;++k){const float* p=source[k].position;
                    d[k]=plane==0?p[3]:plane==1?p[3]+p[0]:plane==2?p[3]-p[0]:plane==3?p[3]+p[1]:p[3]-p[1];magnitude+=std::fabs(d[k]);}
                const float margin=1e-3f*magnitude;
                if(d[0]<-margin&&d[1]<-margin&&d[2]<-margin&&d[3]<-margin){costs.colorCulled+=2;count=0;return false;}
            }
        }
#if defined(TH07_SCREEN_QUAD_CULL) && TH07_SCREEN_QUAD_CULL
        unsigned packedInputs=0;bool screenPrepared=false;float screenXY[4][2];
#endif
        for (unsigned i = 0; i < count; ++i)
        {
            static constexpr unsigned quadSlot[4]={0,1,2,5};
#if defined(TH07_SCREEN_QUAD_CULL) && TH07_SCREEN_QUAD_CULL
            if constexpr(screen)if(directQuads&&i%4==0){
                const auto result=th07_screenquad::prepare(static_cast<const uint8_t*>(data)+size_t(i)*stride,
                    stride,viewX,viewY,viewWidth,viewHeight,screenXY);
                if(result==th07_screenquad::Result::Outside){costs.colorCulled+=2;++costs.earlyScreenQuads;i+=3;continue;}
                screenPrepared=result==th07_screenquad::Result::Prepared;
            }
            const unsigned destination=directQuads?packedInputs++:i;
            Vertex &out=output[directQuads?(destination/4)*6+quadSlot[destination%4]:destination];
#else
            Vertex &out=output[directQuads?(i/4)*6+quadSlot[i%4]:i];
#endif
            const auto *src = static_cast<const uint8_t *>(data) + size_t(i) * stride;
            if constexpr(WordAligned)src=static_cast<const uint8_t*>(__builtin_assume_aligned(src,4));
#if defined(TH07_DUPLICATE_SPRITE_VERTICES) && TH07_DUPLICATE_SPRITE_VERTICES
            if constexpr(screen){
                const bool repeated=type==PRIM_TRIANGLES&&stride==sizeof(VertexTex1DiffuseXyzrhw)&&
                    th07_spritereuse::matchesFixed<sizeof(VertexTex1DiffuseXyzrhw)>(static_cast<const uint8_t*>(data),i);
                if(repeated){
                    out=output[i-2];++costs.duplicateScreenVertices;continue;
                }
            }
#endif
            float position[3]; std::memcpy(position, src, 12);
            uint32_t color; float uv[2] = {0, 0};
            float fogDistance=0;
            if (screen)
            {
                // XYZRHW like Gles.cpp's u_ScreenSpace path: the pixel position and z as given.
                std::memcpy(&color, src + 16, 4);
                if (textured) std::memcpy(uv, src + 20, 8);
#if defined(TH07_SCREEN_QUAD_CULL) && TH07_SCREEN_QUAD_CULL
                if(directQuads&&screenPrepared){out.position[0]=screenXY[i%4][0];out.position[1]=screenXY[i%4][1];}
                else
#endif
                {
#if defined(TH07_SCREEN_AXIS_CACHE) && TH07_SCREEN_AXIS_CACHE
                out.position[0]=xAxis.get(position[0],[&](float x){return (x-viewX)*2/viewWidth-1;});
                out.position[1]=yAxis.get(position[1],[&](float y){return 1-(y-viewY)*2/viewHeight;});
#else
                out.position[0] = (position[0] - viewX) * 2 / viewWidth - 1;
                out.position[1] = 1 - (position[1] - viewY) * 2 / viewHeight;
#endif
                }
                out.position[2] = position[2];
                out.position[3] = 1;
                out.uv[0] = uv[0]; out.uv[1] = uv[1];
                fogDistance = position[2];
            }
            else
            {
                std::memcpy(&color, src + 12, 4); std::memcpy(uv, src + 16, 8);
                const float x = position[0], y = position[1], z = position[2];
                if(!positionPrepared){
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
                // Four columns at once, with the exact scalar multiply/add
                // order and no approximate reciprocal or fused arithmetic.
                const float32x4_t xy=vaddq_f32(vmulq_n_f32(m0,x),vmulq_n_f32(m1,y));
                vst1q_f32(out.position,vaddq_f32(vaddq_f32(xy,vmulq_n_f32(m2,z)),m3));
#else
                out.position[0] = M[0][0] * x + M[1][0] * y + M[2][0] * z + M[3][0];
                out.position[1] = M[0][1] * x + M[1][1] * y + M[2][1] * z + M[3][1];
                out.position[2] = M[0][2] * x + M[1][2] * y + M[2][2] * z + M[3][2];   // D3D: 0 <= z <= w, as GXM
                out.position[3] = M[0][3] * x + M[1][3] * y + M[2][3] * z + M[3][3];
#endif
                }
                // (u, v, 1, 0) * texture matrix, as Gles.cpp's vec4(a_TexCoord, 1.0, 0.0).
                out.uv[0] = uv[0] * T[0][0] + uv[1] * T[1][0] + T[2][0];
                out.uv[1] = uv[0] * T[0][1] + uv[1] * T[1][1] + T[2][1];
                if(distanceFog){
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
                    const auto viewPosition=vaddq_f32(vaddq_f32(vaddq_f32(vmulq_n_f32(v0,x),vmulq_n_f32(v1,y)),vmulq_n_f32(v2,z)),v3);
                    const float vx=vgetq_lane_f32(viewPosition,0),vy=vgetq_lane_f32(viewPosition,1),vz=vgetq_lane_f32(viewPosition,2);
#else
                    const float vx = V[0][0] * x + V[1][0] * y + V[2][0] * z + V[3][0];
                    const float vy = V[0][1] * x + V[1][1] * y + V[2][1] * z + V[3][1];
                    const float vz = V[0][2] * x + V[1][2] * y + V[2][2] * z + V[3][2];
#endif
                    fogDistance = std::sqrt(vx * vx + vy * vy + vz * vz);
                }
            }
#if defined(TH07_VERTEX_COLOUR_CACHE) && TH07_VERTEX_COLOUR_CACHE
            const float* arg=drawTextureArg==TEX_ARG_TFACTOR?factor:drawTextureArg==TEX_ARG_TEXTURE?white:
                vertexColour.get(color,colorFloats);
            if(rgbArg&&alphaArg)std::memcpy(out.diffuse,arg,16);
            else{
            out.diffuse[0]=rgbArg?arg[0]:1;out.diffuse[1]=rgbArg?arg[1]:1;
            out.diffuse[2]=rgbArg?arg[2]:1;out.diffuse[3]=alphaArg?arg[3]:1;
            }
#else
            float arg[4];
            if (drawTextureArg == TEX_ARG_TFACTOR) std::copy(factor, factor + 4, arg);
            else if (drawTextureArg == TEX_ARG_TEXTURE) std::copy(white, white + 4, arg);
            else colorFloats(color, arg);
            out.diffuse[0] = rgbArg ? arg[0] : 1; out.diffuse[1] = rgbArg ? arg[1] : 1;
            out.diffuse[2] = rgbArg ? arg[2] : 1; out.diffuse[3] = alphaArg ? arg[3] : 1;
#endif
#if !defined(TH07_VITA) || defined(TH07_HOSTREF)
            out.specular[0] = out.specular[1] = out.specular[2] = out.specular[3] = 0;
#endif
#if defined(TH07_VERTEX_COLOUR_CACHE) && TH07_VERTEX_COLOUR_CACHE
            if(!distanceFog)std::memcpy(out.fog,identityFog,16);
            else{
#endif
            out.fog[0] = fog[0]; out.fog[1] = fog[1]; out.fog[2] = fog[2];
            out.fog[3] = distanceFog ? (drawFogFar - fogDistance) / fogExtent : 1;
#if defined(TH07_VERTEX_COLOUR_CACHE) && TH07_VERTEX_COLOUR_CACHE
            }
#endif
        }
#if defined(TH07_SCREEN_QUAD_CULL) && TH07_SCREEN_QUAD_CULL
        if(directQuads)count=packedInputs;
#endif
#if defined(TH07_SCREEN_AXIS_CACHE) && TH07_SCREEN_AXIS_CACHE
        if constexpr(screen){costs.screenAxisHits+=xAxis.hits+yAxis.hits;costs.screenAxisMisses+=xAxis.misses+yAxis.misses;}
#endif
        if(directQuads)for(unsigned q=0;q<count/4*6;q+=6){
#if defined(TH07_PACKED_NATIVE_SPRITES) && TH07_PACKED_NATIVE_SPRITES
            th07_spritequad::finishExpanded(expanded.data()+q);
#else
            expanded[q+3]=expanded[q+2];expanded[q+4]=expanded[q+1];
#endif
        }
        return true;
    }
    // `source` as a triangle list (strips/fans/quads expanded).
    bool convert(PrimitiveType type,int primitiveCount,const void* data,int stride,bool screen,bool textured,unsigned& count){
        const bool aligned=!(reinterpret_cast<uintptr_t>(data)&3)&&!(stride&3);
        if(screen)return aligned?convertImpl<true,true>(type,primitiveCount,data,stride,textured,count):
                                 convertImpl<false,true>(type,primitiveCount,data,stride,textured,count);
        return aligned?convertImpl<true,false>(type,primitiveCount,data,stride,textured,count):
                       convertImpl<false,false>(type,primitiveCount,data,stride,textured,count);
    }
    const Vertex *triangleList(PrimitiveType type, unsigned count, unsigned &vertices)
    {
        if(sourceIsQuadTriangles){vertices=count/4*6;return expanded.data();}
        if (type == PRIM_TRIANGLES) { vertices = count; return source.data(); }
        if (type == PRIM_QUADS)
        {
            static const unsigned order[6] = {0, 1, 2, 2, 1, 3};
            vertices=count/4*6;if(expanded.size()<vertices)expanded.resize(vertices);size_t at=0;
            for (unsigned q = 0; q + 3 < count; q += 4)
                for (unsigned k : order) expanded[at++]=source[q + k];
        }
        else
        {
            vertices=count>=3?(count-2)*3:0;if(expanded.size()<vertices)expanded.resize(vertices);size_t at=0;
            for (unsigned i = 0; i + 2 < count; ++i)
            {
                expanded[at++]=type == PRIM_TRIANGLE_FAN ? source[0] : source[i];
                expanded[at++]=source[i + 1];
                expanded[at++]=source[i + 2];
            }
        }
        return expanded.data();
    }
    // Cuts triangles crossing f = 1 or f = 0 (see the top); f is clamped afterwards.
    const Vertex *cutAtFogLimits(const Vertex *vertices, unsigned &count)
    {
        bool any = false;
        for (unsigned i = 0; i < count && !any; i += 3)
        {
            const float a = vertices[i].fog[3], b = vertices[i + 1].fog[3], c = vertices[i + 2].fog[3];
            const float lo = std::min(a, std::min(b, c)), hi = std::max(a, std::max(b, c));
            any = (lo < 1 && hi > 1) || (lo < 0 && hi > 0);
        }
        if (!any)
        {
            if (vertices != source.data() && vertices != expanded.data() && vertices != visible.data()) return vertices;
            Vertex *mutableVertices = const_cast<Vertex *>(vertices);
            for (unsigned i = 0; i < count; ++i) mutableVertices[i].fog[3] = std::min(1.f, std::max(0.f, vertices[i].fog[3]));
            return vertices;
        }
        cut.clear();
        for (unsigned i = 0; i < count; i += 3)
        {
            const float a = vertices[i].fog[3], b = vertices[i + 1].fog[3], c = vertices[i + 2].fog[3];
            const float lo = std::min(a, std::min(b, c)), hi = std::max(a, std::max(b, c));
            if (!((lo < 1 && hi > 1) || (lo < 0 && hi > 0)))
            {
                for (unsigned k = 0; k < 3; ++k)
                {
                    cut.push_back(vertices[i + k]);
                    cut.back().fog[3] = std::min(1.f, std::max(0.f, cut.back().fog[3]));
                }
                continue;
            }
            ++costs.cutTriangles;
            polygon[0].assign(vertices + i, vertices + i + 3);
            // f >= 1, then the rest split at f = 0.
            clipPolygon(polygon[0], 1, +1, polygon[1]);
            clipPolygon(polygon[0], 1, -1, polygon[2]);
            const auto emit = [&](const std::vector<Vertex> &piece) {
                for (size_t k = 1; k + 1 < piece.size(); ++k)
                    for (const Vertex *v : {&piece[0], &piece[k], &piece[k + 1]})
                    {
                        cut.push_back(*v);
                        cut.back().fog[3] = std::min(1.f, std::max(0.f, cut.back().fog[3]));
                    }
            };
            emit(polygon[1]);
            clipPolygon(polygon[2], 0, +1, polygon[1]);
            emit(polygon[1]);
            clipPolygon(polygon[2], 0, -1, polygon[1]);
            emit(polygon[1]);
        }
        count = unsigned(cut.size());
        return cut.data();
    }

    // ---- the draw ----
    const Vertex *staged[5]{};   // geometry, and the multipass inputs (see prepareMultipass)
    bool stagedOk = false;
    bool issue(const Vertex *source, unsigned first, unsigned count, vita2d_texture *texture, const DrawState &state)
    {
        check(stagedOk ? device.drawStaged(source + first, count, texture, state) : device.draw(source + first, count, texture, state));
        return !failed;
    }
    void drawInner(PrimitiveType type, int primitiveCount, const void *data, int stride)
    {
        if (!data || primitiveCount <= 0 || failed) return;
        const bool screen = stride != sizeof(VertexTex1DiffuseXyz);
        const bool textured = stride != sizeof(VertexDiffuseXyzrhw);
        if (stride != sizeof(VertexTex1DiffuseXyz) && stride != sizeof(VertexTex1DiffuseXyzrhw) && stride != sizeof(VertexDiffuseXyzrhw)) return;
        const uint64_t convertStart = fineUs();
        unsigned count = 0, vertices = 0;
        // Both components DISABLE on a textured draw: the colour is the argument alone.
        const bool sampled = textured && !(rgbOp == COLOR_OP_DISABLE && alphaOp == COLOR_OP_DISABLE);
        if (sampled && (rgbOp == COLOR_OP_ADD || alphaOp == COLOR_OP_ADD || rgbOp == COLOR_OP_DISABLE || alphaOp == COLOR_OP_DISABLE))
            ++costs.unsupportedOps;   // never issued by TH07; drawn as MODULATE
        if(!convert(type, primitiveCount, data, stride, screen, sampled, count)){
            const auto elapsed=fineUs()-convertStart;costs.transformUs+=elapsed;costs.convertUs+=elapsed;return;
        }
        const Vertex *list = triangleList(type, count, vertices);
        const uint64_t transformed=fineUs();costs.transformUs+=transformed-convertStart;
        if (vertices < 3 || vertices > 65535) { costs.convertUs += fineUs() - convertStart; return; }
        // Entirely offscreen colour/depth triangles need neither fog splitting
        // nor multipass preparation. Preserve visible triangle order, and use
        // the same conservative homogeneous-plane margin as depth rejection.
        unsigned firstHidden=0;
        while(firstHidden+2<vertices&&!outsideDepthViewport(list+firstHidden))firstHidden+=3;
        if(firstHidden+2<vertices){
#if defined(TH07_CPU_SCRATCH_REUSE) && TH07_CPU_SCRATCH_REUSE
            size_t visibleCount=0;
            th07_scratch::copy(visible,visibleCount,list,firstHidden);
            for(unsigned at=firstHidden;at+2<vertices;at+=3){
                if(outsideDepthViewport(list+at))++costs.colorCulled;
                else th07_scratch::copy(visible,visibleCount,list+at,3);
            }
            vertices=unsigned(visibleCount);list=visible.data();
#else
            visible.assign(list,list+firstHidden);
            for(unsigned at=firstHidden;at+2<vertices;at+=3){
                if(outsideDepthViewport(list+at))++costs.colorCulled;
                else visible.insert(visible.end(),list+at,list+at+3);
            }
            vertices=unsigned(visible.size());list=visible.data();
#endif
            if(!vertices){costs.convertUs+=fineUs()-convertStart;return;}
        }
        const uint64_t visibleTime=fineUs();costs.visibilityUs+=visibleTime-transformed;
        // convertImpl writes fog.w = exactly 1 when distance fog is disabled.
        // Triangle expansion and visibility filtering only copy those values,
        // so the scan and per-vertex clamp would both be identity operations.
        // Keep the original clipping path whenever range fog can be active.
        if (fogEnabled && fogFar - fogNear != 0)
            list = cutAtFogLimits(list, vertices);
        costs.fogCutUs+=fineUs()-visibleTime;
        if (vertices > 65535) vertices = 65535 / 3 * 3;
        costs.vertices += vertices;
        costs.convertUs += fineUs() - convertStart;
        ++costs.gameDraws;
        // Consecutive game draws with the same GPU state go to the GPU together (the stage
        // backgrounds draw each quad separately: ~150 fogged quads a frame on stage 5).
        if (!pending.empty() && (pendingSampled != sampled || pending.size() + vertices > kBatchVertices)) flushPending();
        if (pending.empty()) {pendingSampled = sampled;pendingAllSimple=sourceAllSimple;}
        else pendingAllSimple&=sourceAllSimple;
        pending.insert(pending.end(), list, list + vertices);
    }
    // ---- batches: GL draws them one by one with this state, in this order ----
    static constexpr size_t kBatchVertices = 48000;
    std::vector<Vertex> pending;
    bool pendingSampled = false, insideDraw = false;
    bool pendingAllSimple=false;
    void beforeStateChange() { if (!pending.empty()) flushPending(); }
    void flushPending()
    {
        if (pending.empty()) return;
        const uint64_t start = nowUs();
        if (!failed) drawBatch(pending.data(), unsigned(pending.size()), pendingSampled,pendingAllSimple);
        pending.clear();
        ++costs.batches;
        if (!insideDraw) drawUsThisFrame += nowUs() - start;
    }
    void drawBatch(const Vertex *list, unsigned vertices, bool sampled,bool simpleNative=false)
    {
        DrawState state = baseState();
        if (!sampled)
        {
            // Diffuse program: the colour and the fog are per vertex (exact: the colour is
            // uniform in every TH07 untextured draw; fog is linear in f).
            lit.assign(list, list + vertices);
            for (auto &v : lit)
                for (unsigned k = 0; k < 3; ++k) v.diffuse[k] = v.fog[k] + (v.diffuse[k] - v.fog[k]) * v.fog[3];
            state.program = Program::Diffuse;
            stagedOk = false;
            issue(lit.data(), 0, vertices, nullptr, state);
            return;
        }
        Texture *texture = boundTexture();
        vita2d_texture *image = texture ? texture->image : nullptr;
        if (!image) ++costs.unboundTextureDraws;
        if (texture && image && !(powerOfTwo(texture->width) && powerOfTwo(texture->height)))
        { state.repeatU = state.repeatV = false; state.addressU = state.addressV = 2; }   // GXM wraps only POT
        else state.repeatU = state.repeatV = true;
        // Depth under the alpha test: colour without depth, then alphaTestedDepth (unless every
        // texel of the texture passes the test at the draw's smallest vertex alpha). Only where a
        // later draw tests against it: depth written under DEPTH_FUNC_ALWAYS (everything after
        // the stage background) is cleared before the next LEQUAL draw (Stage::OnDraw).
        bool testedDepth = state.depthWrite && state.depthFunc != SCE_GXM_DEPTH_FUNC_ALWAYS && alphaTest && alphaRef &&
                           texture && image && !texture->cellAlpha.empty();
        if (testedDepth)
        {
            float alpha = 1;
            for (unsigned i = 0; i < vertices; ++i) alpha = std::min(alpha, list[i].diffuse[3]);
            testedDepth = !(float(texture->minAlpha) * alpha >= float(alphaRef) - 1e-3f);
        }
        // A batch with several pass groups, or depth under the alpha test, is drawn by levels.
        unsigned multipass = 0;
        if (!testedDepth&&!simpleNative)
            for (unsigned i = 0; i + 2 < vertices && multipass < 8; i += 3)
            {
                const Vertex *v = list + i;
                multipass += std::min(v[0].fog[3], std::min(v[1].fog[3], v[2].fog[3])) < kFogOne ||
                             !sameRgba(v[0].diffuse, v[1].diffuse) || !sameRgba(v[0].diffuse, v[2].diffuse);
            }
        if (!testedDepth && multipass < 8) { drawTextured(list, vertices, image, texture, state,simpleNative); return; }
        // GL writes each fragment's depth as it draws it, so a later triangle (the stage 1 trees
        // are one sprite batch) is already hidden behind an earlier, nearer one: with the alpha
        // test, a level's colour goes first (depth-tested, not written), then its depth.
        DrawState colorState = state;
        if (testedDepth) colorState.depthWrite = false;
        const uint64_t scheduleStart = fineUs();
        schedule(list, vertices / 3);
        costs.scheduleUs += fineUs() - scheduleStart;
        unsigned first = 0;
        for (unsigned end : levelEnds)
        {
            if (failed) break;
            drawTextured(ordered.data() + first * 3, (end - first) * 3, image, texture, colorState,simpleNative);
            if (testedDepth && !failed) alphaTestedDepth(ordered.data() + first * 3, (end - first) * 3, *texture, state);
            first = end;
        }
        ++costs.scheduledBatches;
        costs.levels += levelEnds.size();
    }
    // Levels: a triangle's level is one more than the highest level of an earlier triangle it
    // overlaps on screen (one crossing the eye plane overlaps everything). No two triangles of a
    // level overlap, so a level may be drawn in any order: sorted by colour and fog, its runs and
    // pass groups (which keep a value per pixel in the target's alpha) are as long as possible,
    // while every overlapping pair keeps GL's order. On stage 5 the fogged stairs overlap step by
    // step: grouping consecutive triangles only gave ~130 pass groups (~900 GPU draws) a frame.
    std::vector<ScreenTriangle> levelTriangles;
    std::vector<unsigned> levelOf, levelTested, levelOrder;
    std::vector<uint64_t> levelKey;
    std::vector<int> levelGrid[16 * 16], levelTouched;
    std::vector<Vertex> ordered;
    std::vector<unsigned> levelEnds;
    unsigned levelStamp = 1;
    void schedule(const Vertex *list, unsigned triangles)
    {
        constexpr int G = 16;
        const auto cellRange = [](float lo, float hi, int &a, int &b) {
            a = std::max(0, std::min(G - 1, int((lo + 1.f) * .5f * G)));
            b = std::max(0, std::min(G - 1, int((hi + 1.f) * .5f * G)));
        };
        levelTriangles.resize(triangles);
        levelOf.assign(triangles, 0);
        levelKey.resize(triangles);
        if (levelTested.size() < triangles) levelTested.resize(triangles, 0);
        for (int cell : levelTouched) levelGrid[cell].clear();
        levelTouched.clear();
        unsigned floor = 0, top = 0;
        for (unsigned t = 0; t < triangles; ++t)
        {
            const Vertex *v = list + t * 3;
            const ScreenTriangle tri = screenTriangle(v);
            levelTriangles[t] = tri;
            unsigned level = floor;
            if (tri.unbounded)
            {
                if (t) level = std::max(floor, top + 1);
                floor = level + 1;
            }
            else
            {
                int x0, x1, y0, y1;
                cellRange(tri.minX, tri.maxX, x0, x1); cellRange(tri.minY, tri.maxY, y0, y1);
                ++levelStamp;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        for (int other : levelGrid[y * G + x])
                        {
                            if (levelTested[other] == levelStamp) continue;
                            levelTested[other] = levelStamp;
                            if (levelOf[other] + 1 > level && overlaps(tri, levelTriangles[other])) level = levelOf[other] + 1;
                        }
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        auto &cell = levelGrid[y * G + x];
                        if (cell.empty()) levelTouched.push_back(y * G + x);
                        cell.push_back(int(t));
                    }
            }
            levelOf[t] = level;
            top = std::max(top, level);
            // Colour and fog class: equal keys end up next to each other inside a level.
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < 3; ++k)
            {
                const auto *b = reinterpret_cast<const uint8_t *>(v[k].diffuse);
                for (int i = 0; i < 16; ++i) h = (h ^ b[i]) * 1099511628211ull;
                h = (h ^ uint8_t(v[k].fog[3] < kFogOne)) * 1099511628211ull;
            }
            levelKey[t] = h;
        }
        levelOrder.resize(triangles);
        for (unsigned t = 0; t < triangles; ++t) levelOrder[t] = t;
        std::stable_sort(levelOrder.begin(), levelOrder.end(), [&](unsigned a, unsigned b) {
            return levelOf[a] != levelOf[b] ? levelOf[a] < levelOf[b] : levelKey[a] < levelKey[b];
        });
        ordered.resize(size_t(triangles) * 3);
        levelEnds.clear();
        for (unsigned i = 0; i < triangles; ++i)
        {
            const unsigned t = levelOrder[i];
            std::copy(list + t * 3, list + t * 3 + 3, ordered.begin() + size_t(i) * 3);
            if (i + 1 == triangles || levelOf[levelOrder[i + 1]] != levelOf[t]) levelEnds.push_back(i + 1);
        }
    }
    void drawTextured(const Vertex *list, unsigned vertices, vita2d_texture *image, const Texture *texture, const DrawState &base,bool simpleNative=false)
    {
        const uint64_t prepareStart = fineUs();
        const unsigned triangles = vertices / 3;
        if(!simpleNative)info.resize(triangles);
        else {++costs.simpleNativeBatches;costs.simpleNativeVertices+=vertices;}
        const bool alphaMatters = base.blend && (base.colorSrc == SCE_GXM_BLEND_FACTOR_SRC_ALPHA ||
                                                 base.colorDst == SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        // With src alpha 1 the game's blend keeps nothing of the target: SRC_ALPHA or ONE over
        // INV_SRC_ALPHA or ZERO (or no blend).
        const bool opaqueBlend = !base.blend || ((base.colorSrc == SCE_GXM_BLEND_FACTOR_SRC_ALPHA || base.colorSrc == ONE) &&
                                                 (base.colorDst == SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA || base.colorDst == ZERO));
        bool anyMultipass = false;
        for (unsigned t = 0; !simpleNative&&t < triangles; ++t)
        {
            const Vertex *v = list + t * 3;
            const float fogMin = std::min(v[0].fog[3], std::min(v[1].fog[3], v[2].fog[3]));
            const bool rgbSame = sameRgb(v[0].diffuse, v[1].diffuse) && sameRgb(v[0].diffuse, v[2].diffuse);
            const bool alphaSame = !alphaMatters || (v[0].diffuse[3] == v[1].diffuse[3] && v[0].diffuse[3] == v[2].diffuse[3]);
            uint8_t kind = !rgbSame ? RgbVaries : !alphaSame ? AlphaVaries : fogMin < kFogOne ? Fogged : Plain;
            if (kind == Fogged && opaqueBlend && texture && image && !texture->cellAlpha.empty() &&
                v[0].diffuse[3] == 1.f && v[1].diffuse[3] == 1.f && v[2].diffuse[3] == 1.f && opaqueTexels(v, *texture))
                kind = FoggedOpaque;
            if (kind == RgbVaries) ++costs.rgbGradientTriangles;
            else if (kind == AlphaVaries) ++costs.alphaGradientTriangles;
            info[t] = {kind};
            anyMultipass |= kind != Plain;
        }
#if defined(TH07_PLAIN_RUN_REORDER) && TH07_PLAIN_RUN_REORDER
        if(!anyMultipass&&vertices>=192&&plainOrder.build(list,vertices)){
            plainOrdered.clear();
            for(unsigned r:plainOrder.order){const auto& run=plainOrder.runs[r];plainOrdered.insert(plainOrdered.end(),list+run.first,list+run.first+run.count);}
            plainMoved+=plainOrder.moved;list=plainOrdered.data();
        }
#endif
        // The geometry goes to GPU memory once; every run and pass draws from that copy.
        const void* compactGeometry=nullptr;
#if defined(TH07_SIMPLE_UV_STREAM) && TH07_SIMPLE_UV_STREAM
        if(simpleNative){compactGeometry=device.stageProvenUV(list,vertices);if(compactGeometry)costs.compactUvVertices+=vertices;}
#endif
        staged[0] = compactGeometry?nullptr:device.stage(list, vertices);
        stagedOk = staged[0] != nullptr;
        if (anyMultipass) prepareMultipass(list, vertices, stagedOk ? staged[0] : list);
        const Vertex *geometry = stagedOk ? staged[0] : list;   // after prepareMultipass: it may fall back
        costs.prepareUs += fineUs() - prepareStart;

        unsigned t = 0;
        while (t < triangles && !failed)
        {
            if (simpleNative||info[t].kind == Plain)
            {
                // A run of one colour: texel * tint with the game's blend.
                const unsigned start = t;
                const float *color = list[t * 3].diffuse;
                while (t < triangles && (simpleNative||info[t].kind == Plain) && sameRgba(list[t * 3].diffuse, color)) ++t;
                DrawState pass = base;
                pass.program = Program::TextureTint;
                std::copy(color, color + 4, pass.tint);
                ++costs.plainRuns; ++costs.passes;
#if defined(TH07_SIMPLE_UV_STREAM) && TH07_SIMPLE_UV_STREAM
                if(compactGeometry)check(device.drawCompactStaged(static_cast<const uint8_t*>(compactGeometry)+start*3*24,
                    (t-start)*3,image,pass,Device::Layout::UV));
                else
#endif
                issue(geometry, start * 3, (t - start) * 3, image, pass);
                continue;
            }
            t = multipassGroup(list, t, triangles, image, base);
        }
    }

    // Same cellAlpha == 255 bounding-box decision as cutTriangle(kind == 1),
    // without tessellating every passing cell merely to return a yes/no answer.
    bool opaqueTexels(const Vertex *v, const Texture &texture)
    {
        if (texture.minAlpha == 255) return true;
        float tx[3],ty[3];
        for(int k=0;k<3;++k){tx[k]=v[k].uv[0]*texture.width;ty[k]=v[k].uv[1]*texture.height;}
        const float minX=std::min({tx[0],tx[1],tx[2]}),maxX=std::max({tx[0],tx[1],tx[2]});
        const float minY=std::min({ty[0],ty[1],ty[2]}),maxY=std::max({ty[0],ty[1],ty[2]});
        if(!(maxX-minX<65536.f&&maxY-minY<65536.f)||
           !(std::fabs(minX)<1e8f&&std::fabs(maxX)<1e8f&&std::fabs(minY)<1e8f&&std::fabs(maxY)<1e8f))return false;
        const float det=(tx[1]-tx[0])*(ty[2]-ty[0])-(tx[2]-tx[0])*(ty[1]-ty[0]);
        const int x0=int(std::floor(minX/kCell)),x1=int(std::floor(maxX/kCell));
        const int y0=int(std::floor(minY/kCell)),y1=int(std::floor(maxY/kCell));
        if(int64_t(x1-x0+1)*(y1-y0+1)>65536||std::fabs(det)<1e-6f)return false;
        const int width=int(texture.cellsX),height=int(texture.cellsY),stride=width+1;
        if(!width||!height)return false;
        auto& prefix=texture.opaquePrefix;
        if(texture.opaquePrefixVersion!=texture.version)
        {
            prefix.assign(size_t(stride)*(height+1),0);
            for(int y=0;y<height;++y)for(int x=0;x<width;++x){
                const size_t at=size_t(y+1)*stride+x+1;
                prefix[at]=uint32_t(texture.cellAlpha[size_t(y)*width+x]!=255)+prefix[at-1]+prefix[at-stride]-prefix[at-stride-1];
            }
            texture.opaquePrefixVersion=texture.version;
        }
        const bool wrap=powerOfTwo(texture.width)&&powerOfTwo(texture.height);
        int xr[2][2],yr[2][2];
        const auto ranges=[&](int lo,int hi,int extent,int (*out)[2]){
            if(!wrap){out[0][0]=std::max(0,std::min(extent-1,lo));out[0][1]=std::max(0,std::min(extent-1,hi))+1;return 1;}
            const int count=hi-lo+1;
            if(count>=extent){out[0][0]=0;out[0][1]=extent;return 1;}
            const int begin=((lo%extent)+extent)%extent,end=begin+count;
            out[0][0]=begin;out[0][1]=std::min(extent,end);
            if(end<=extent)return 1;
            out[1][0]=0;out[1][1]=end-extent;return 2;
        };
        const int nx=ranges(x0,x1,width,xr),ny=ranges(y0,y1,height,yr);
        for(int y=0;y<ny;++y)for(int x=0;x<nx;++x){
            const uint32_t sum=prefix[size_t(yr[y][1])*stride+xr[x][1]]+prefix[size_t(yr[y][0])*stride+xr[x][0]]-
                prefix[size_t(yr[y][0])*stride+xr[x][1]]-prefix[size_t(yr[y][1])*stride+xr[x][0]];
            if(sum)return false;
        }
        return true;
    }
    // Multipass inputs: [1] alpha = f (rgb 0), [2] rgb = fog colour * (1 - f) (alpha 1),
    // [3] alpha = vertex alpha (rgb 0), [4] rgb = fog colour * (1 - f), alpha = f.
    const Vertex *inputs[5]{};
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
    std::vector<touhou::modernvita::Colour> colourAux[4];
    const touhou::modernvita::Colour* colourInputs[4]{};
    bool compactMultipass=false;
#endif
    void prepareMultipass(const Vertex *list, unsigned vertices, const Vertex *geometry)
    {
        inputs[0] = geometry;
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
        compactMultipass=true;
        for(unsigned i=0;i<vertices&&compactMultipass;++i)
            for(float f:list[i].fog)compactMultipass&=std::isfinite(f);
        if(compactMultipass){
#if defined(TH07_SPARSE_MULTIPASS_COLOURS) && TH07_SPARSE_MULTIPASS_COLOURS
            costs.multipassInputVertices+=vertices;
            for(unsigned t=0;t<vertices/3;++t)if(info[t].kind!=Plain)costs.multipassUsedVertices+=3;
#endif
            for(unsigned k=0;k<4;++k){
                auto& array=colourAux[k];
                if(array.size()<vertices)array.resize(vertices);
                for(unsigned i=0;i<vertices;++i){
#if defined(TH07_SPARSE_MULTIPASS_COLOURS) && TH07_SPARSE_MULTIPASS_COLOURS
                    // Plain triangles draw only the original geometry. A
                    // multipass group terminates before every Plain triangle,
                    // so none of these retained auxiliary slots is consumed.
                    if(info[i/3].kind==Plain)continue;
#endif
                    array[i]=th07_multipass::colour(list[i],k);
                }
                const auto* copy=stagedOk?device.stageColours(array.data(),vertices):nullptr;
                if(stagedOk&&!copy)stagedOk=false;
                colourInputs[k]=copy;
            }
            if(!stagedOk){
                inputs[0]=list;
                for(unsigned k=0;k<4;++k)colourInputs[k]=colourAux[k].data();
            }
            return;
        }
#endif
        for (unsigned k = 0; k < 4; ++k)
        {
            auto &array = aux[k];
            array.assign(list, list + vertices);
            for (auto &v : array)
            {
                const float f = v.fog[3];
                if (k == 0) { v.diffuse[0] = v.diffuse[1] = v.diffuse[2] = 0; v.diffuse[3] = f; }
                else if (k == 1) { for (unsigned c = 0; c < 3; ++c) v.diffuse[c] = v.fog[c] * (1 - f); v.diffuse[3] = 1; }
                else if (k == 2) { v.diffuse[0] = v.diffuse[1] = v.diffuse[2] = 0; }
                else { for (unsigned c = 0; c < 3; ++c) v.diffuse[c] = v.fog[c] * (1 - f); v.diffuse[3] = f; }
            }
            const Vertex *copy = stagedOk ? device.stage(array.data(), vertices) : nullptr;
            if (stagedOk && !copy) stagedOk = false;   // this frame's vertex memory is full: copy per pass
            staged[k + 1] = copy;
        }
        if (stagedOk) for (unsigned k = 1; k < 5; ++k) inputs[k] = staged[k];
        else { inputs[0] = list; for (unsigned k = 1; k < 5; ++k) inputs[k] = aux[k - 1].data(); }
    }

    // Groups of consecutive non-plain triangles of one kind and tint that do not overlap on
    // screen, binned in a 16x16 grid so a triangle is only tested against its neighbours
    // (TH06: comparing with the whole group was quadratic).
    static constexpr int kGrid = 16;
    std::vector<ScreenTriangle> group;
    std::vector<int> grid[kGrid * kGrid], touched;
    std::vector<unsigned> tested;
    unsigned stamp = 1;
    unsigned multipassGroup(const Vertex *list, unsigned t, unsigned triangles, vita2d_texture *image, const DrawState &base)
    {
        const auto cellRange = [](float lo, float hi, int &first, int &last) {
            first = std::max(0, std::min(kGrid - 1, int((lo + 1.f) * .5f * kGrid)));
            last = std::max(0, std::min(kGrid - 1, int((hi + 1.f) * .5f * kGrid)));
        };
        group.clear();
        for (int cell : touched) grid[cell].clear();
        touched.clear();
        const unsigned start = t;
        const uint8_t kind = info[t].kind == RgbVaries ? AlphaVaries : info[t].kind;
        float tint[4];
        groupTint(list + t * 3, tint);
        bool fogged = false, term = false;
        for (; t < triangles; ++t)
        {
            const uint8_t k = info[t].kind == RgbVaries ? AlphaVaries : info[t].kind;
            if (k == Plain || k != kind) break;
            float other[4]; groupTint(list + t * 3, other);
            if (kind == Fogged || kind == FoggedOpaque ? !sameRgba(other, tint) : !sameRgb(other, tint)) break;
            const ScreenTriangle current = screenTriangle(list + t * 3);
            bool clash = current.unbounded && !group.empty();
            int x0 = 0, x1 = kGrid - 1, y0 = 0, y1 = kGrid - 1;
            if (!current.unbounded)
            {
                ++stamp;
                cellRange(current.minX, current.maxX, x0, x1); cellRange(current.minY, current.maxY, y0, y1);
                for (int y = y0; y <= y1 && !clash; ++y)
                    for (int x = x0; x <= x1 && !clash; ++x)
                        for (int other : grid[y * kGrid + x])
                        {
                            if (tested[other] == stamp) continue;
                            tested[other] = stamp;
                            if (overlaps(current, group[other])) { clash = true; break; }
                        }
            }
            if (!clash && !group.empty() && group.back().unbounded) clash = true;
            if (clash) break;
            const int index = int(group.size());
            group.push_back(current);
            if (tested.size() < group.size()) tested.resize(group.size(), 0);
            tested[index] = 0;
            if (!current.unbounded)
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                    {
                        auto &cell = grid[y * kGrid + x];
                        if (cell.empty()) touched.push_back(y * kGrid + x);
                        cell.push_back(index);
                    }
            const Vertex *v = list + t * 3;
            for (unsigned k = 0; k < 3; ++k)
            {
                fogged |= v[k].fog[3] < kFogOne;
                for (unsigned c = 0; c < 3; ++c) term |= v[k].fog[c] * (1 - v[k].fog[3]) > 0.5f / 255.f;
            }
        }
        if (t == start) ++t;   // cannot happen (the first triangle always joins); never loop forever
        ++costs.groups;
        if (kind == FoggedOpaque) opaqueFogPasses(start * 3, (t - start) * 3, image, base, tint);
        else if (kind == Fogged) fogPasses(start * 3, (t - start) * 3, image, base, tint, term);
        else alphaPasses(start * 3, (t - start) * 3, image, base, tint, fogged, term);
        return t;
    }
    // The tint of a triangle in a multipass group: its colour (an RGB gradient, never issued by
    // TH07, takes the average of its vertices and is counted in the costs line).
    static void groupTint(const Vertex *v, float *tint)
    {
        for (unsigned c = 0; c < 4; ++c) tint[c] = v[0].diffuse[c];
        if (!(sameRgb(v[0].diffuse, v[1].diffuse) && sameRgb(v[0].diffuse, v[2].diffuse)))
            for (unsigned c = 0; c < 3; ++c) tint[c] = (v[0].diffuse[c] + v[1].diffuse[c] + v[2].diffuse[c]) / 3;
    }

    struct Pass {
        int input;            // inputs[] index: 0 geometry (texel * tint), 1 alpha = f, 2 rgb = fog term, 3 alpha = vertex alpha
        Program program;
        uint32_t mask;
        bool blend;
        SceGxmBlendFactor colorSrc, colorDst, alphaSrc, alphaDst;
    };
    void runPasses(const Pass *passes, unsigned count, unsigned first, unsigned vertices, vita2d_texture *image,
                   const DrawState &base, const float *tint)
    {
        // Depth-tested alike; depth is written by the last pass only.
        for (unsigned i = 0; i < count && !failed; ++i)
        {
            const Pass &p = passes[i];
            DrawState state = base;
            state.program = p.program;
            std::copy(tint, tint + 4, state.tint);
            state.colorMask = p.mask;
            state.blend = p.blend;
            state.colorBlend = state.alphaBlend = SCE_GXM_BLEND_FUNC_ADD;
            state.colorSrc = p.colorSrc; state.colorDst = p.colorDst;
            state.alphaSrc = p.alphaSrc; state.alphaDst = p.alphaDst;
            state.depthWrite = i + 1 == count && base.depthWrite;
            ++costs.passes;
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
            if(compactMultipass&&p.input){
                const auto* colours=colourInputs[p.input-1]+first;
                check(stagedOk?device.drawColoursStaged(colours,vertices,state):device.drawColours(colours,vertices,state));
            }else
#endif
            issue(inputs[p.input], first, vertices, p.program == Program::TextureTint ? image : nullptr, state);
        }
    }
    static constexpr auto ONE = SCE_GXM_BLEND_FACTOR_ONE, ZERO = SCE_GXM_BLEND_FACTOR_ZERO;
    static constexpr auto DST_ALPHA = SCE_GXM_BLEND_FACTOR_DST_ALPHA, INV_DST_ALPHA = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    // Opaque fogged (src alpha 1, the target is not kept): GL's colour is mix(fog, texel*c, f).
    //   rgb := fog*(1-f), A := f   (vertex colour, no blend: the variant of the Device's clear)
    //   rgb += texel*c * A
    void opaqueFogPasses(unsigned first, unsigned vertices, vita2d_texture *image, const DrawState &base, const float *tint)
    {
        const Pass passes[2] = {{4, Program::Diffuse, SCE_GXM_COLOR_MASK_ALL, false, ONE, ZERO, ONE, ZERO},
                                {0, Program::TextureTint, kMaskRgb, true, DST_ALPHA, ONE, ONE, ZERO}};
        ++costs.opaqueFogGroups;
        runPasses(passes, 2, first, vertices, image, base, tint);
    }
    // Uniform tint (c, a), fogged: TH06's sequence (run on a real Vita), with s = texel.a * a:
    //   A := f;  A *= s;  rgb := texel*c * A + D*X;  A := s;  rgb += fog*(1-f) * A
    // = (texel*c*f + fog*(1-f)) * s + D*X, X the game's destination factor. Without the
    // source alpha factor (ONE, ZERO): A := f; rgb := texel*c * A; rgb += fog*(1-f).
    void fogPasses(unsigned first, unsigned vertices, vita2d_texture *image, const DrawState &base, const float *tint, bool term)
    {
        Pass passes[5]; unsigned n = 0;
        passes[n++] = {1, Program::Diffuse, kMaskA, false, ONE, ZERO, ONE, ZERO};
        if (base.blend && base.colorSrc == SCE_GXM_BLEND_FACTOR_SRC_ALPHA)
        {
            passes[n++] = {0, Program::TextureTint, kMaskA, true, ONE, ZERO, DST_ALPHA, ZERO};
            passes[n++] = {0, Program::TextureTint, kMaskRgb, true, DST_ALPHA, base.colorDst, ONE, ZERO};
            if (term)
            {
                passes[n++] = {0, Program::TextureTint, kMaskA, false, ONE, ZERO, ONE, ZERO};
                passes[n++] = {2, Program::Diffuse, kMaskRgb, true, DST_ALPHA, ONE, ONE, ZERO};
            }
        }
        else
        {
            const auto destination = base.blend ? base.colorDst : ZERO;
            passes[n++] = {0, Program::TextureTint, kMaskRgb, true, DST_ALPHA, destination, ONE, ZERO};
            if (term) passes[n++] = {2, Program::Diffuse, kMaskRgb, true, ONE, ONE, ONE, ZERO};
        }
        runPasses(passes, n, first, vertices, image, base, tint);
    }
    // Vertex alpha a_v varies (the enemy trails), RGB c uniform, s = texel.a * a_v:
    //   A := a_v;  A *= texel.a                      (A = s)
    //   unfogged:  rgb := texel*c * A + D*X          (X = 1 - A, 1 or 0)
    //   fogged:    rgb := fog*(1-f) * A + D*X;  A *= f;  rgb += texel*c * A
    // = (texel*c*f + fog*(1-f)) * s + D*X.
    void alphaPasses(unsigned first, unsigned vertices, vita2d_texture *image, const DrawState &base, const float *color,
                     bool fogged, bool term)
    {
        const float tint[4] = {color[0], color[1], color[2], 1};
        Pass passes[6]; unsigned n = 0;
        if (!(base.blend && base.colorSrc == SCE_GXM_BLEND_FACTOR_SRC_ALPHA))
        {
            // The source alpha does not weight the colour (never with a varying alpha in TH07):
            // as fogPasses, with the first vertex's alpha.
            ++costs.unsupportedOps;
            fogPasses(first, vertices, image, base, color, term);
            return;
        }
        const auto destination = base.colorDst == SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ? INV_DST_ALPHA : base.colorDst;
        passes[n++] = {3, Program::Diffuse, kMaskA, false, ONE, ZERO, ONE, ZERO};
        passes[n++] = {0, Program::TextureTint, kMaskA, true, ONE, ZERO, DST_ALPHA, ZERO};
        if (!fogged)
            passes[n++] = {0, Program::TextureTint, kMaskRgb, true, DST_ALPHA, destination, ONE, ZERO};
        else
        {
            if (term || destination != ONE)
                passes[n++] = {2, Program::Diffuse, kMaskRgb, true, term ? DST_ALPHA : ZERO, destination, ONE, ZERO};
            passes[n++] = {1, Program::Diffuse, kMaskA, true, ONE, ZERO, DST_ALPHA, ZERO};
            passes[n++] = {0, Program::TextureTint, kMaskRgb, true, DST_ALPHA, ONE, ONE, ZERO};
        }
        runPasses(passes, n, first, vertices, image, base, tint);
    }

    // Depth where GL's alpha test passes: texel alpha * vertex alpha >= alphaRef/255 for every
    // texel a fragment can sample (TH07's MODULATE alpha). A triangle whose reachable cells all
    // pass writes depth as a whole (its own vertices, so the same depth values as its colour);
    // one with some passing cells writes depth over those cells only: runs of passing cells,
    // merged across rows into rectangles in texture space, are cut to the triangle in its
    // barycentric coordinates (texture coordinates are affine over a triangle) and mapped to
    // clip space with them. The cut depends only on the triangle's texture coordinates, the
    // texture and the threshold, not on where the triangle is: it is cached in barycentric form
    // (the same sprites and background tiles come back every frame).
    struct CutKey {
        uint32_t version, threshold;
        float uv[6];
        bool operator==(const CutKey &o) const { return std::memcmp(this, &o, sizeof(CutKey)) == 0; }
    };
    struct CutKeyHash {
        size_t operator()(const CutKey &k) const
        {
            uint64_t h = 1469598103934665603ull;
            const auto *b = reinterpret_cast<const uint8_t *>(&k);
            for (size_t i = 0; i < sizeof(CutKey); ++i) h = (h ^ b[i]) * 1099511628211ull;
            return size_t(h);
        }
    };
    struct Cut { uint8_t kind; std::vector<float> bary; size_t active=0;
#if defined(TH07_DEPTH_FAN_REUSE) && TH07_DEPTH_FAN_REUSE
        std::vector<uint8_t> fans;size_t fanActive=0;
#endif
    };   // kind 0 none, 1 whole, 2 bary triangles
    std::unordered_map<CutKey, Cut, CutKeyHash> cuts;
    struct CutSight { size_t hash=0; unsigned frame=~0u; } cutSight[4096]{};
    Cut transientCut;
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
    struct DepthVertex { float position[4]; };
#else
    using DepthVertex=Vertex;
#endif
    std::vector<DepthVertex> depthVertices;
    struct Bary { float b[3]; };
    struct Open { int x0, x1, y0; };
    std::vector<Open> open, nextOpen;
    std::vector<std::pair<int,int>> rowPassRuns;
#if defined(TH07_THRESHOLD_CACHE) && TH07_THRESHOLD_CACHE
    th07_depthruns::Cache<Texture::DepthRuns> thresholdRuns;
    uint64_t thresholdHits=0,thresholdMisses=0;
#endif
    const Texture::DepthRuns &ensureDepthRuns(const Texture &texture, uint32_t threshold)
    {
        const bool reference4=threshold==texture.nextCellAlpha[4];
        auto* selected=&texture.depthRuns[reference4?0:1];
#if defined(TH07_THRESHOLD_CACHE) && TH07_THRESHOLD_CACHE
        // Rows-only entries: no multi-MiB prefix atlas. The worst-case RLE
        // capacity of two <=1M-cell grids stays below 18 MiB globally;
        // real retained capacity and the C heap are reported every 600 frames.
        // Large/nonstandard textures keep the existing single-atlas fallback.
        if(!reference4&&texture.cellsX<=4096&&texture.cellsY<=4096&&
           uint64_t(texture.cellsX)*texture.cellsY<=1048576){
            auto entry=thresholdRuns.get(texture.version,threshold);
            if(entry.hit){++thresholdHits;return entry.data;}
            ++thresholdMisses;selected=&entry.data;selected->rowsOnly=true;
        }
#endif
        auto& cache=*selected;
        if(cache.version==texture.version&&cache.threshold==threshold)return cache;
        const auto start=nowUs();
        const int width=int(texture.cellsX),height=int(texture.cellsY),stride=width+1;
        auto& prefix=cache.prefix;auto& rows=cache.rows;
        if(cache.rowWidth!=texture.cellsX){rows.clear();cache.rowWidth=texture.cellsX;}
        if(cache.rowsOnly)th07_depthruns::buildRows(rows,texture.cellAlpha.data(),width,height,threshold);
        else{
        prefix.assign(size_t(stride)*(height+1),0);rows.resize(height);
        for(int y=0;y<height;++y){
            rows[y].clear();
            int begin=-1;
            for(int x=0;x<width;++x){
                const bool pass=texture.cellAlpha[size_t(y)*width+x]>=threshold;
                if(!cache.rowsOnly){
                    const size_t at=size_t(y+1)*stride+x+1;
                    prefix[at]=uint32_t(pass)+prefix[at-1]+prefix[at-stride]-prefix[at-stride-1];
                }
                if(pass&&begin<0)begin=x;
                if(!pass&&begin>=0){rows[y].emplace_back(begin,x);begin=-1;}
            }
            if(begin>=0)rows[y].emplace_back(begin,width);
        }
        }
        cache.version=texture.version;cache.threshold=threshold;
        const uint64_t elapsed=nowUs()-start;
        costs.depthRunsBuildUs+=elapsed;++costs.depthRunsBuilds;
#if defined(TH07_THRESHOLD_CACHE) && TH07_THRESHOLD_CACHE
        if(costs.depthRunsBuilds<=4)th07_vita_log("GXM atlas rebuild frame=%u version=%u threshold=%u cells=%ux%u rows_only=%d ms=%.3f",
            presentedFrames,texture.version,threshold,texture.cellsX,texture.cellsY,int(cache.rowsOnly),elapsed/1000.0);
#endif
        return cache;
    }
    static int cellRanges(int lo,int hi,int extent,bool wrap,int (*out)[2])
    {
        if(!wrap){out[0][0]=std::max(0,std::min(extent-1,lo));out[0][1]=std::max(0,std::min(extent-1,hi))+1;return 1;}
        const int count=hi-lo+1;
        if(count>=extent){out[0][0]=0;out[0][1]=extent;return 1;}
#if defined(TH07_DEPTH_POWER2_WRAP) && TH07_DEPTH_POWER2_WRAP
        const int begin=powerOfTwo(unsigned(extent))?int(th07_cellwrap::remainder(lo,unsigned(extent))):((lo%extent)+extent)%extent;
        const int end=begin+count;
#else
        const int begin=((lo%extent)+extent)%extent,end=begin+count;
#endif
        out[0][0]=begin;out[0][1]=std::min(extent,end);
        if(end<=extent)return 1;
        out[1][0]=0;out[1][1]=end-extent;return 2;
    }
    void cutTriangle(const Vertex *v, const Texture &texture, uint32_t threshold, Cut &cut)
    {
        cut.kind = 0;
        cut.active=0;
#if defined(TH07_DEPTH_FAN_REUSE) && TH07_DEPTH_FAN_REUSE
        cut.fanActive=0;
#endif
#if !defined(TH07_CPU_SCRATCH_REUSE) || !TH07_CPU_SCRATCH_REUSE
        cut.bary.clear();
#endif
        const float W = float(texture.width), H = float(texture.height);
        const bool wrap = powerOfTwo(texture.width) && powerOfTwo(texture.height);
        const int cellsX = int(texture.cellsX), cellsY = int(texture.cellsY);
        float tx[3], ty[3];
        for (int k = 0; k < 3; ++k) { tx[k] = v[k].uv[0] * W; ty[k] = v[k].uv[1] * H; }
        const float minX = std::min({tx[0], tx[1], tx[2]}), maxX = std::max({tx[0], tx[1], tx[2]});
        const float minY = std::min({ty[0], ty[1], ty[2]}), maxY = std::max({ty[0], ty[1], ty[2]});
        if (!(maxX - minX < 65536.f && maxY - minY < 65536.f)) { ++costs.depthUnmapped; return; }
        const int cx0 = int(std::floor(minX / kCell)), cx1 = int(std::floor(maxX / kCell));
        const int cy0 = int(std::floor(minY / kCell)), cy1 = int(std::floor(maxY / kCell));
        const float du1 = tx[1] - tx[0], du2 = tx[2] - tx[0], dv1 = ty[1] - ty[0], dv2 = ty[2] - ty[0];
        const float det = du1 * dv2 - du2 * dv1;
        if (int64_t(cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 65536 || std::fabs(det) < 1e-6f)
        {
            ++costs.depthUnmapped;   // a sliver or a huge repeat: no depth (never a hole)
            return;
        }
        const auto& runsCache=ensureDepthRuns(texture,threshold);
        {
            // Exact any/all answers for EVERY reference, including vertex-alpha
            // fades in stage 6. The reference-4-only fast path never applied to
            // those draws: each missed triangle rescanned up to 65K cells.
            int xr[2][2],yr[2][2];
            const int nx=cellRanges(cx0,cx1,cellsX,wrap,xr),ny=cellRanges(cy0,cy1,cellsY,wrap,yr),stride=cellsX+1;
            const auto& prefix=runsCache.prefix;bool any=false,all=true;
            if(runsCache.rowsOnly)th07_depthruns::classify(runsCache.rows,xr,nx,yr,ny,any,all);
            else for(int y=0;y<ny;++y)for(int x=0;x<nx;++x){
                const uint32_t sum=prefix[size_t(yr[y][1])*stride+xr[x][1]]+prefix[size_t(yr[y][0])*stride+xr[x][0]]-
                    prefix[size_t(yr[y][0])*stride+xr[x][1]]-prefix[size_t(yr[y][1])*stride+xr[x][0]];
                any|=sum!=0;all&=sum==uint32_t((yr[y][1]-yr[y][0])*(xr[x][1]-xr[x][0]));
            }
            if(!any)return;if(all){cut.kind=1;return;}
        }
        cut.kind = 2;
        const auto toBary = [&](float x, float y) {
            const float px = x - tx[0], py = y - ty[0];
            Bary b;
            b.b[1] = (dv2 * px - du2 * py) / det;
            b.b[2] = (du1 * py - dv1 * px) / det;
            b.b[0] = 1 - b.b[1] - b.b[2];
            return b;
        };
        // Rectangles meet in T-junctions and at the quad's diagonal, where separately computed
        // edges can leave hairline cracks: each grows by 1/64 texel and is cut to the triangle
        // with a small tolerance (a sliver of depth at most, never a crack).
        constexpr float grow = 1.f / 64, edge = -2e-4f;
        const auto emit = [&](int x0, int x1, int y0, int y1)
#if defined(TH07_DEPTH_RECT_OUTLINE) && TH07_DEPTH_RECT_OUTLINE
            __attribute__((noinline,hot))
#endif
        {   // cells [x0, x1) x [y0, y1)
            const float u0 = float(x0 * int(kCell)) - grow, u1 = float(x1 * int(kCell)) + grow;
            const float w0 = float(y0 * int(kCell)) - grow, w1 = float(y1 * int(kCell)) + grow;
            // Three clipping planes can at most double the vertex count per
            // plane (4 -> 8 -> 16 -> 32), even for boundary duplicates.
            // Keep the exact former walk/intersection order in fixed arrays.
            Bary work[2][32];
            Bary *rect = work[0], *clipped = work[1];
            rect[0]=toBary(u0,w0);rect[1]=toBary(u1,w0);rect[2]=toBary(u1,w1);rect[3]=toBary(u0,w1);
            size_t count=4;
            th07_baryclip::triangle(rect,clipped,count,edge);
            if(count<3)return;
#if defined(TH07_DEPTH_FAN_REUSE) && TH07_DEPTH_FAN_REUSE
            *th07_scratch::append(cut.fans,cut.fanActive,1)=uint8_t(count);
#endif
#if defined(TH07_CPU_SCRATCH_REUSE) && TH07_CPU_SCRATCH_REUSE
            float* output=th07_scratch::append(cut.bary,cut.active,(count-2)*9);
#else
            const size_t first=cut.bary.size();cut.bary.resize(first+(count-2)*9);
            float *output=cut.bary.data()+first;
#endif
            for (size_t i = 1; i + 1 < count; ++i)
                for (const Bary *b : {&rect[0], &rect[i], &rect[i + 1]}) {
                    std::memcpy(output,b->b,3*sizeof(float));output+=3;
                }
        };
        // Runs of passing cells per row; a run identical to one in the row above extends it.
        open.clear();
#if defined(TH07_DEPTH_POWER2_WRAP) && TH07_DEPTH_POWER2_WRAP
        const bool powerWrap=wrap&&powerOfTwo(unsigned(cellsX))&&powerOfTwo(unsigned(cellsY));
        const int firstTile=powerWrap?th07_cellwrap::floorTile(cx0,unsigned(cellsX)):0;
        const int lastTile=powerWrap?th07_cellwrap::floorTile(cx1,unsigned(cellsX)):0;
#endif
        for (int cy = cy0; cy <= cy1 + 1; ++cy)
        {
            nextOpen.clear();
            size_t o = 0;
            rowPassRuns.clear();
            const auto appendRun=[&](int first,int last){
                first=std::max(first,cx0);last=std::min(last,cx1+1);if(first>=last)return;
                if(!rowPassRuns.empty()&&rowPassRuns.back().second==first)rowPassRuns.back().second=last;
                else rowPassRuns.emplace_back(first,last);
            };
            if(cy<=cy1){
#if defined(TH07_DEPTH_POWER2_WRAP) && TH07_DEPTH_POWER2_WRAP
                const int row=powerWrap?int(th07_cellwrap::remainder(cy,unsigned(cellsY))):
                    wrap?((cy%cellsY)+cellsY)%cellsY:std::max(0,std::min(cellsY-1,cy));
#else
                const int row=wrap?((cy%cellsY)+cellsY)%cellsY:std::max(0,std::min(cellsY-1,cy));
#endif
                const auto& runs=runsCache.rows[row];
                if(wrap){
#if defined(TH07_DEPTH_POWER2_WRAP) && TH07_DEPTH_POWER2_WRAP
                    const int startTile=powerWrap?firstTile:(cx0-((cx0%cellsX)+cellsX)%cellsX)/cellsX;
                    const int endTile=powerWrap?lastTile:(cx1-((cx1%cellsX)+cellsX)%cellsX)/cellsX;
#else
                    const int startTile=(cx0-((cx0%cellsX)+cellsX)%cellsX)/cellsX;
                    const int endTile=(cx1-((cx1%cellsX)+cellsX)%cellsX)/cellsX;
#endif
                    for(int tile=startTile;tile<=endTile;++tile){
                        const int offset=tile*cellsX;
#if defined(TH07_DEPTH_RUN_WINDOW) && TH07_DEPTH_RUN_WINDOW
                        th07_runwindow::visit(runs,cx0-offset,cx1+1-offset,
                            [&](const auto& run){appendRun(offset+run.first,offset+run.second);});
#else
                        for(const auto& run:runs)appendRun(offset+run.first,offset+run.second);
#endif
                    }
                }else{
                    if(!runs.empty()&&runs.front().first==0)appendRun(cx0,0);
#if defined(TH07_DEPTH_RUN_WINDOW) && TH07_DEPTH_RUN_WINDOW
                    th07_runwindow::visit(runs,cx0,cx1+1,
                        [&](const auto& run){appendRun(run.first,run.second);});
#else
                    for(const auto& run:runs)appendRun(run.first,run.second);
#endif
                    if(!runs.empty()&&runs.back().second==cellsX)appendRun(cellsX,cx1+1);
                }
            }
            for(const auto& run:rowPassRuns)
            {
                const int x0=run.first,cx=run.second;
                while (o < open.size() && open[o].x0 < x0) { emit(open[o].x0, open[o].x1, open[o].y0, cy); ++o; }
                if (o < open.size() && open[o].x0 == x0 && open[o].x1 == cx) nextOpen.push_back(open[o++]);
                else nextOpen.push_back({x0, cx, cy});
            }
            for (; o < open.size(); ++o) emit(open[o].x0, open[o].x1, open[o].y0, cy);
            open.swap(nextOpen);
        }
    }
    static bool outsideDepthViewport(const Vertex *v)
    {
        // If even one vertex is inside all tested planes, this triangle cannot
        // be wholly outside any of them. Most sprite triangles take this path.
        for(int k=0;k<3;++k){
            const float* p=v[k].position;
#if defined(TH07_VIEWPORT_INSIDE_BITS) && TH07_VIEWPORT_INSIDE_BITS
            if(th07_viewinside::inside(p))return false;
#else
            if(p[3]>0&&std::fabs(p[0])<=p[3]&&std::fabs(p[1])<=p[3])return false;
#endif
        }
        // Same homogeneous viewport rejection as modern VitaDrawPlan, but
        // keep a margin for this adapter's -2e-4 barycentric clipping tolerance.
        // Visible/crossing triangles still take the unmodified depth-mask path.
        for (int plane = 0; plane < 5; ++plane) {
            float distance[3], magnitude = 1;
            for (int k = 0; k < 3; ++k) {
                const auto *p = v[k].position;
                distance[k] = plane == 0 ? p[3] : plane == 1 ? p[3] + p[0] :
                              plane == 2 ? p[3] - p[0] : plane == 3 ? p[3] + p[1] : p[3] - p[1];
                magnitude += std::fabs(distance[k]);
            }
            const float margin = 1e-3f * magnitude;
            if (distance[0] < -margin && distance[1] < -margin && distance[2] < -margin) return true;
        }
        return false;
    }
    void alphaTestedDepth(const Vertex *list, unsigned vertices, const Texture &texture, const DrawState &base)
    {
        const uint64_t start = fineUs();
        // Keep a zero-initialized high-water pool. This pass writes only depth:
        // draw_v passes position through, Diffuse has no discard/depth output,
        // colourMask=0 and blending is disabled. Other attributes remain zero.
        size_t depthCount=0;
        if (cuts.size() > 8192) cuts.clear();
        for (unsigned t = 0; t + 2 < vertices; t += 3)
        {
            const Vertex *v = list + t;
            if (outsideDepthViewport(v)) { ++costs.depthCulled; continue; }
            const float alpha = std::min(v[0].diffuse[3], std::min(v[1].diffuse[3], v[2].diffuse[3]));
            // A cell passes when its smallest reachable texel alpha * alpha >= alphaRef.
            const uint32_t rawThreshold = alpha > 0 ? uint32_t(std::ceil(float(alphaRef) / alpha - 1e-3f)) : 256u;
            const uint32_t threshold=rawThreshold<=255?texture.nextCellAlpha[rawThreshold]:256;
            uint8_t kind;
            const Cut *cut = nullptr;
            if (texture.minAlpha >= threshold) kind = 1;
            else if (threshold > 255) kind = 0;
            else
            {
                CutKey key;
                std::memset(&key, 0, sizeof(key));
                key.version = texture.version; key.threshold = threshold;
                for (int k = 0; k < 3; ++k) { key.uv[k * 2] = v[k].uv[0]; key.uv[k * 2 + 1] = v[k].uv[1]; }
                auto found = cuts.find(key);
                if (found == cuts.end())
                {
                    ++costs.depthCellTriangles;
                    if (rawThreshold == 4) ++costs.depthThreshold4; else ++costs.depthThresholdOther;
                    // Fog-limit intersections move their UVs every frame.
                    // Do not retain thousands of one-use barycentric arrays
                    // then free them all in an 8192-entry cache-clear hitch.
                    // The admission fingerprint affects caching only: map hits
                    // still compare the COMPLETE key, and every miss is computed.
                    const size_t hash=CutKeyHash{}(key);
                    auto& sight=cutSight[hash%4096];
                    const bool repeated=sight.hash==hash&&sight.frame<=presentedFrames&&presentedFrames-sight.frame<=4;
                    sight={hash,presentedFrames};
                    Cut* computed=repeated?&cuts.emplace(key,Cut{}).first->second:&transientCut;
                    const uint64_t cutStart = fineUs();
                    cutTriangle(v, texture, threshold, *computed);
                    costs.depthCutUs += fineUs() - cutStart;
                    cut=computed;
                }
                else cut = &found->second;
                kind = cut->kind;
            }
            if (kind == 1) {
                if(depthVertices.size()<depthCount+3)depthVertices.resize(depthCount+3);
                for(int k=0;k<3;++k){std::memcpy(depthVertices[depthCount+k].position,v[k].position,4*sizeof(float));
#if !defined(TH07_VITA) || defined(TH07_HOSTREF)
                    depthVertices[depthCount+k].fog[3]=1;
#endif
                }
                depthCount+=3;continue;
            }
            if (kind == 0) continue;
            const size_t first = depthCount;
#if defined(TH07_CPU_SCRATCH_REUSE) && TH07_CPU_SCRATCH_REUSE
            const size_t count=cut->active/3;
#else
            const size_t count=cut->bary.size()/3;
#endif
            // Zero-initialize once and fill in place. Avoid a 72-byte temporary
            // copy plus vector growth check for each tessellated depth vertex.
            if(depthVertices.size()<first+count)depthVertices.resize(first+count);
            depthCount+=count;
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
            const float32x4_t p0=vld1q_f32(v[0].position),p1=vld1q_f32(v[1].position),p2=vld1q_f32(v[2].position);
#endif
            const auto transformPosition=[&](size_t i){
                const float *b = &cut->bary[i * 3];
                auto &out = depthVertices[first + i];
#if defined(__ARM_NEON) && !defined(TH07_HOSTREF)
                // Separate multiply/add intrinsics preserve the scalar's two
                // roundings and left-associative sum (-ffp-contract=off).
                // TH07's inherited FPSCR has FZ/DN enabled on both paths.
                const float32x4_t a=vaddq_f32(vmulq_n_f32(p0,b[0]),vmulq_n_f32(p1,b[1]));
                vst1q_f32(out.position,vaddq_f32(a,vmulq_n_f32(p2,b[2])));
#else
                for (int j = 0; j < 4; ++j)
                    out.position[j] = b[0] * v[0].position[j] + b[1] * v[1].position[j] + b[2] * v[2].position[j];
#endif
#if !defined(TH07_VITA) || defined(TH07_HOSTREF)
                out.fog[3] = 1;
#endif
            };
#if defined(TH07_DEPTH_FAN_REUSE) && TH07_DEPTH_FAN_REUSE && !defined(TH07_HOSTREF)
            costs.depthFanReused+=th07_depthfan::expand(depthVertices.data()+first,count,
                cut->fans.data(),cut->fanActive,transformPosition);
#else
            for(size_t i=0;i<count;++i)transformPosition(i);
#endif
        }
        DrawState state = base;
        state.program = Program::Diffuse;
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
        // No discard/depth output in either proven program. Colour is masked
        // off, so Uniform writes exactly the same depth with a packed stream.
        state.program=Program::Uniform;std::fill(state.tint,state.tint+4,0);
#endif
        state.colorMask = 0;
        state.blend = false;
        state.depthWrite = true;
        costs.depthVertices += depthCount;
        const uint64_t prepareUs = fineUs() - start;
        costs.prepareUs += prepareUs;
        costs.depthPrepareUs += prepareUs;
        stagedOk = false;
        // The 16-bit index limit allows 65,535 vertices, but at 72 bytes each
        // that exceeds Device's 4 MiB transient ring even after it is rewound.
        // Stage 6 reaches that depth-mask size at frame 351. Preserve every
        // triangle and its order in the same 48,000-vertex batches as colour.
        constexpr size_t depthDrawVertices = kBatchVertices / 3 * 3;
        static_assert(depthDrawVertices * sizeof(Vertex) <= (4u << 20), "depth draw fits the GPU ring");
        for (size_t first = 0; first < depthCount && !failed; first += depthDrawVertices)
        {
            const unsigned count = unsigned(std::min<size_t>(depthDrawVertices, depthCount - first));
            ++costs.passes;
#if defined(TH07_VITA) && !defined(TH07_HOSTREF)
            static_assert(sizeof(DepthVertex)==sizeof(touhou::modernvita::Position),"packed depth positions");
            check(device.drawPositions(reinterpret_cast<const touhou::modernvita::Position*>(depthVertices.data()+first),count,state));
#else
            issue(depthVertices.data() + first, 0, count, nullptr, state);
#endif
        }
    }

public:
    void SwapBuffers() override
    {
        beforeStateChange();
        if (failed || !frame) return;
        if (failed || !device.target(nullptr)) { check(false); return; }
        device.viewport(0, 0, 960, 544);
        const float black[4] = {0, 0, 0, 1};
        check(device.clear(true, false, black, 1));
#ifdef TH07_VITA
        const float width = fitOutput ? 544.0f * 640 / 480 : 960.0f;
#else
        const float width = g_Supervisor.cfg.windowed ? 544.0f * 640 / 480 : 960.0f;
#endif
        device.viewport((960 - width) / 2, 0, width, 544);
        Vertex quad[4]{};
        for (unsigned j = 0; j < 4; ++j)
        {
            quad[j].position[0] = (j & 1) ? 1.f : -1.f; quad[j].position[1] = (j & 2) ? -1.f : 1.f;
            quad[j].position[3] = 1;
            quad[j].uv[0] = (j & 1) ? 1.f : 0.f; quad[j].uv[1] = (j & 2) ? 1.f : 0.f;
            std::fill(quad[j].diffuse, quad[j].diffuse + 4, 1.f); quad[j].fog[3] = 1;
        }
        DrawState display;
        display.primitive = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
        display.blend = false;
        display.program = Program::TextureTint;   // texel * white
        check(device.draw(quad, 4, frame, display));
        const uint64_t presentStart = nowUs();
        if (!device.present()) { check(false); return; }
        ++presentedFrames;
        noteFrame(presentStart);
#ifndef TH07_VITA
        th07_vita_presented();
        sampleDrawnState();
#endif
        check(device.target(frame)); applyViewport();
    }
};
}
#ifdef TH07_VITA
#include "AsyncGraphics.inc"
#endif
ZunGraphics *Th07CreateVitaBackend()
{
#ifdef TH07_VITA
    auto *graphics = new AsyncGraphics;
#else
    auto *graphics = new VitaGraphics;
#endif
    if (!graphics->initialize()) { delete graphics; return nullptr; }
    return graphics;
}
