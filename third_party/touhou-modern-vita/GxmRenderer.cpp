#include "GxmRenderer.hpp"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstdarg>
#ifdef TOUHOU_VITA_TRACE_CLEANUP
extern "C" void touhou_vita_trace_fini(int active);
#endif

namespace {
// Teardown trace points; they write nothing in this build.
void lifetimeNote(const char*,...){}
}

namespace touhou::vita {
struct DepthBuffer {
    SceGxmDepthStencilSurface surface{};
    SceUID uid=0;
    void *memory=nullptr;
    uint32_t width=0,height=0;
};
struct Device::Impl {
    bool initialized = false, scene = false;
    std::string failure;
    SceGxmContext *ctx = nullptr;
    SceGxmShaderPatcher *patcher = nullptr;
    std::vector<uint32_t> vertexBytes, fragmentBytes;
    SceGxmShaderPatcherId vertexId = nullptr, fragmentId = nullptr;
    SceGxmVertexProgram *vertexProgram = nullptr;
    const SceGxmProgramParameter *params[4]{};
    // The proven programs (DrawState::Program Uniform, Diffuse, TextureTint), when the package has them.
    struct Proven { const char *file, *uniform; std::vector<uint32_t> bytes; SceGxmShaderPatcherId id = nullptr; const SceGxmProgramParameter *tint = nullptr; };
    Proven proven[4] = {{"proven_uniform_f.gxp", "uClearColor"}, {"proven_diffuse_f.gxp", nullptr}, {"proven_tint_f.gxp", "tint"},
                        {"texmad_f.gxp", nullptr}};
    std::map<uint64_t, SceGxmFragmentProgram *> fragments;   // (program << 32) | blend
    vita2d_texture *target = nullptr, *white = nullptr;
    DepthBuffer *depth = nullptr;
    float viewport[6] = {0, 0, 960, 544, 0, 1};
    Device::Statistics statistics;
    // Frame pipelining (2026-10-13): present() no longer waits for the GPU (it did, every
    // frame: CPU and GPU took turns, 40-50 fps on a Vita that runs the PSP's 60 easily). The
    // display scene of frame n ends with a fragment notification writing n into `fence`.
    // Vertex data goes to two rings used by alternate frames, a ring being reused only after
    // the frame that filled it completed; a texture last drawn in frame m can be written or
    // reused once the fence reads m. finish() still waits for everything (readbacks etc.).
    volatile unsigned int *fence = nullptr; uint32_t frameSerial = 1;   // the frame being built
    struct Ring { uint8_t *base = nullptr; SceUID uid = -1; uint32_t size = 0, used = 0, frame = 0; };
    Ring rings[2]; unsigned ring = 0;
    std::map<vita2d_texture *, uint32_t> lastUse;   // frame that last sampled or drew into it
    // Destroyed (non-target) textures kept for a createTexture of the same size: menus and
    // the title made, filled and destroyed a 1024x512 texture every frame.
    struct Cached { vita2d_texture *texture; uint32_t width, height, bytes, frame; };
    std::vector<Cached> textureCache; uint32_t cacheBytes = 0;
    static constexpr uint32_t cacheLimit = 16u << 20;
    // Storage replaced by a whole-texture upload while the GPU could still read it (text, menus):
    // the draws already recorded keep the old SceGxmTexture words, so the new pixels go to fresh
    // memory and the old block is reused for the next such upload once its frame has finished.
    struct Spare { SceUID uid; void *memory; uint32_t bytes, frame; };
    std::vector<Spare> spares; uint32_t spareBytes = 0;
    static constexpr uint32_t spareLimit = 8u << 20;
    void *allocateBlock(uint32_t bytes, bool cdram, SceUID &uid, bool zero) {
        const uint32_t alignment=cdram ? 256*1024 : 4096;
        const uint32_t allocation=(bytes+alignment-1)&~(alignment-1);
        SceUID block=sceKernelAllocMemBlock("touhou-gxm",cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,allocation,nullptr);
        if (block<0) return nullptr;
        void *memory=nullptr;
        if (sceKernelGetMemBlockBase(block,&memory)<0 ||
            sceGxmMapMemory(memory,allocation,static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ|SCE_GXM_MEMORY_ATTRIB_WRITE))<0) {
            sceKernelFreeMemBlock(block); return nullptr;
        }
        uid=block; if (zero) memset(memory,0,allocation); return memory;
    }
    void freeBlock(SceUID uid, void *memory) { sceGxmUnmapMemory(memory); sceKernelFreeMemBlock(uid); }
    bool replaceStorage(vita2d_texture *t, uint32_t frame) {
        const uint32_t width = vita2d_texture_get_width(t), height = vita2d_texture_get_height(t);
        const uint32_t bytes = vita2d_texture_get_stride(t) * height;
        SceUID uid = -1; void *memory = nullptr;
        for (size_t i = 0; i < spares.size(); ++i) {
            if (spares[i].bytes != bytes || !idle(spares[i].frame)) continue;
            uid = spares[i].uid; memory = spares[i].memory; spareBytes -= bytes; spares.erase(spares.begin() + i); break;
        }
        if (!memory) memory = allocateBlock(bytes, true, uid, false);
        if (!memory) memory = allocateBlock(bytes, false, uid, false);
        if (!memory) return false;
        spares.push_back({t->data_UID, vita2d_texture_get_datap(t), bytes, frame}); spareBytes += bytes;
        t->data_UID = uid;
        sceGxmTextureInitLinear(&t->gxm_tex, memory, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, width, height, 0);
        ++statistics.replacedUploads;
        for (size_t i = 0; spareBytes > spareLimit && i < spares.size();) {
            if (!idle(spares[i].frame)) { ++i; continue; }
            freeBlock(spares[i].uid, spares[i].memory); spareBytes -= spares[i].bytes; spares.erase(spares.begin() + i);
        }
        return true;
    }
    bool submitted = false;
    bool idle(uint32_t frame) const { return !frame || int32_t(*fence - frame) >= 0; }
    void finishAll() {
        end();
        if (submitted) {
            const uint64_t start = sceKernelGetProcessTimeWide();
            sceGxmFinish(ctx); ++statistics.gpuWaits; submitted = false;
            noteWait(sceKernelGetProcessTimeWide() - start);
        }
        lastUse.clear(); for (auto &r : rings) r.frame = 0; for (auto &c : textureCache) c.frame = 0;
        for (auto &spare : spares) spare.frame = 0;
    }
    void noteWait(uint64_t us) {
        statistics.waitUs += us;
        if (us > statistics.longestWaitUs) statistics.longestWaitUs = us;
    }
    void waitFrame(uint32_t frame) {   // everything frame `frame` drew is done
        if (!frame || idle(frame)) return;
        if (frame >= frameSerial) { finishAll(); return; }   // the frame being built: end it here
        ++statistics.gpuWaits;
        const uint64_t start = sceKernelGetProcessTimeWide();
        for (unsigned spins = 0; !idle(frame); ++spins) {
            if (spins == 5000) { fenceTimeouts++; ++statistics.fenceTimeouts; noteWait(sceKernelGetProcessTimeWide() - start); finishAll(); return; }   // 0.5 s: never hang on a missing notification
            sceKernelDelayThread(100);
        }
        noteWait(sceKernelGetProcessTimeWide() - start);
    }
    unsigned fenceTimeouts = 0;
    void waitTexture(vita2d_texture *t) { auto it = lastUse.find(t); if (it != lastUse.end()) waitFrame(it->second); }
    void *transient(uint32_t bytes) {
        Ring &r = rings[ring]; const uint32_t at = (r.used + 15u) & ~15u;
        if (!r.base || at + bytes > r.size) return nullptr;
        r.used = at + bytes; r.frame = frameSerial; return r.base + at;
    }
    void trimCache() {
        while (cacheBytes > cacheLimit && !textureCache.empty()) {
            const Cached oldest = textureCache.front(); textureCache.erase(textureCache.begin());
            waitFrame(oldest.frame); cacheBytes -= oldest.bytes; vita2d_free_texture(oldest.texture);
        }
    }
    bool fail(const char *what, int code = 0) {
        if (failure.empty()) { char message[256]; snprintf(message, sizeof(message), "%s (%08x)", what, unsigned(code)); failure = message; fprintf(stderr, "[vita GXM] %s\n", message); }
        return false;
    }
    bool check(int result, const char *what) { return result >= 0 || fail(what, result); }
    bool load(const std::string &path, std::vector<uint32_t> &data) {
        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp) return fail(path.c_str());
        fseek(fp, 0, SEEK_END); const long bytes = ftell(fp); rewind(fp);
        if (bytes < 64 || bytes > 1024 * 1024) { fclose(fp); return fail("invalid GXP size"); }
        data.resize((bytes + 3) / 4);
        const bool read = fread(data.data(), 1, bytes, fp) == size_t(bytes); fclose(fp);
        return read && check(sceGxmProgramCheck(reinterpret_cast<SceGxmProgram *>(data.data())), "GXP program check");
    }
    void applyViewport() {
        const float *v = viewport;
        sceGxmSetViewport(ctx, v[0] + v[2] * .5f, v[2] * .5f,
                         v[1] + v[3] * .5f, -v[3] * .5f, v[4], v[5] - v[4]);
    }
    // What draw() last set in this scene: the multi-pass fog issues a few hundred draws a frame,
    // and setting all of it again for each one was most of the CPU cost.
    struct BoundState {
        bool programsAndModes = false;
        const SceGxmFragmentProgram *fragment = nullptr;
        int cull = -1, depthFunc = -1, depthWrite = -1;
        SceGxmTexture texture{}; bool textureSet = false;
    } bound;
    bool begin() {
        if (!initialized || !failure.empty()) return false;
        if (!scene) {
            // Do not call vita2d_start_drawing: it rewinds the transient stream
            // while previous scenes can still be reading it.
            if (target && depth) {
                if (!check(sceGxmBeginScene(ctx,0,target->gxm_rtgt,nullptr,nullptr,nullptr,
                    &target->gxm_sfc,&depth->surface),"begin shared-depth scene")) return false;
            } else vita2d_start_drawing_advanced(target, 0);
            scene = true;
            submitted = true;
            if (target) lastUse[target] = frameSerial;
            ++statistics.scenes;
            bound = BoundState();   // set everything again in a new scene
            sceGxmSetRegionClip(ctx, SCE_GXM_REGION_CLIP_NONE, 0, 0, 0, 0);
            sceGxmSetFrontStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                                     SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 255, 255);
            sceGxmSetBackStencilFunc(ctx, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP,
                                    SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 255, 255);
            applyViewport();
        }
        return true;
    }
    void end() {
        if (scene) {
            vita2d_end_drawing(); scene = false;
            // A texture update or target switch can split a game's frame into
            // multiple tile scenes. Preserve its depth rather than silently
            // clearing it at the next BeginScene.
            if (depth) sceGxmDepthStencilSurfaceSetForceLoadMode(&depth->surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            else if (target) sceGxmDepthStencilSurfaceSetForceLoadMode(&target->gxm_sfd, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        }
    }
    SceGxmFragmentProgram *fragment(const DrawState &s) {
        const unsigned which = unsigned(s.program);
        if (which && !proven[which - 1].id) { fail("proven fragment program not in this package"); return nullptr; }
        SceGxmBlendInfo blend{};
        blend.colorMask = s.colorMask;
        blend.colorFunc = s.blend ? s.colorBlend : SCE_GXM_BLEND_FUNC_NONE;
        blend.alphaFunc = s.blend ? s.alphaBlend : SCE_GXM_BLEND_FUNC_NONE;
        blend.colorSrc = s.colorSrc; blend.colorDst = s.colorDst;
        blend.alphaSrc = s.alphaSrc; blend.alphaDst = s.alphaDst;
        uint32_t blendKey; memcpy(&blendKey, &blend, 4);
        const uint64_t key = (uint64_t(which) << 32) | blendKey;
        auto found = fragments.find(key); if (found != fragments.end()) return found->second;
        SceGxmFragmentProgram *program = nullptr;
        if (!check(sceGxmShaderPatcherCreateFragmentProgram(patcher, which ? proven[which - 1].id : fragmentId,
            SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, &blend,
            reinterpret_cast<const SceGxmProgram *>(vertexBytes.data()), &program), "create fragment program")) return nullptr;
        fragments[key] = program; return program;
    }
};
Device::Device() : impl(new Impl) {}
Device::~Device() {
    lifetimeNote("Device dtor begin initialized=%u scene=%u submitted=%u fragments=%u",unsigned(impl->initialized),unsigned(impl->scene),unsigned(impl->submitted),unsigned(impl->fragments.size()));
    if (impl->initialized) {
        lifetimeNote("Device finish begin");
        finish();
        lifetimeNote("Device finish end; white texture free begin");
        for (auto &c : impl->textureCache) vita2d_free_texture(c.texture);
        impl->textureCache.clear();
        for (auto &spare : impl->spares) impl->freeBlock(spare.uid, spare.memory);
        impl->spares.clear();
        for (auto &r : impl->rings) if (r.base) { sceGxmUnmapMemory(r.base); sceKernelFreeMemBlock(r.uid); r.base = nullptr; }
        if (impl->white) vita2d_free_texture(impl->white);
        lifetimeNote("Device white texture free end; fragment release begin");
        for (auto &f : impl->fragments) {
            lifetimeNote("Device fragment release key=%08x ptr=%p begin",f.first,static_cast<void*>(f.second));
            const int rc=sceGxmShaderPatcherReleaseFragmentProgram(impl->patcher, f.second);
            lifetimeNote("Device fragment release key=%08x end rc=%08x",f.first,unsigned(rc));
        }
        lifetimeNote("Device vertex program release begin");
        if (impl->vertexProgram) sceGxmShaderPatcherReleaseVertexProgram(impl->patcher, impl->vertexProgram);
        lifetimeNote("Device vertex program release end; unregister fragment begin");
        if (impl->fragmentId) sceGxmShaderPatcherUnregisterProgram(impl->patcher, impl->fragmentId);
        for (auto &p : impl->proven) if (p.id) sceGxmShaderPatcherUnregisterProgram(impl->patcher, p.id);
        lifetimeNote("Device unregister fragment end; unregister vertex begin");
        if (impl->vertexId) sceGxmShaderPatcherUnregisterProgram(impl->patcher, impl->vertexId);
        lifetimeNote("Device unregister vertex end; vita2d_fini begin");
#ifdef TOUHOU_VITA_TRACE_CLEANUP
        touhou_vita_trace_fini(1);
#endif
        vita2d_fini();
#ifdef TOUHOU_VITA_TRACE_CLEANUP
        touhou_vita_trace_fini(0);
#endif
        lifetimeNote("Device vita2d_fini end");
    }
    delete impl;
    lifetimeNote("Device dtor end");
}
bool Device::initialize(const char *directory) {
    auto &d = *impl;
    if (d.initialized) return d.failure.empty();
    if (!d.load(std::string(directory) + "/draw_v.gxp", d.vertexBytes) ||
        !d.load(std::string(directory) + "/draw_f.gxp", d.fragmentBytes)) return false;
    if (vita2d_init_advanced(8 * 1024 * 1024) <= 0) return d.fail("vita2d initialize");
    d.initialized = true; d.ctx = vita2d_get_context(); d.patcher = vita2d_get_shader_patcher();
    vita2d_set_vblank_wait(1);
    auto *vp = reinterpret_cast<const SceGxmProgram *>(d.vertexBytes.data());
    auto *fp = reinterpret_cast<const SceGxmProgram *>(d.fragmentBytes.data());
    if (!d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, vp, &d.vertexId), "register vertex") ||
        !d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, fp, &d.fragmentId), "register fragment")) return false;
    const char *names[] = {"position", "diffuse", "uv", "specular", "fog"};
    const uint16_t offsets[] = {offsetof(Vertex,position),offsetof(Vertex,diffuse),offsetof(Vertex,uv),offsetof(Vertex,specular),offsetof(Vertex,fog)};
    const uint8_t components[] = {4,4,2,4,4};
    SceGxmVertexAttribute attributes[5]{};
    for (unsigned i = 0; i < 5; ++i) {
        const auto *p = sceGxmProgramFindParameterByName(vp, names[i]);
        if (!p) return d.fail(names[i]);
        attributes[i] = {0, offsets[i], SCE_GXM_ATTRIBUTE_FORMAT_F32, components[i], uint16_t(sceGxmProgramParameterGetResourceIndex(p))};
    }
    const SceGxmVertexStream stream{sizeof(Vertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    if (!d.check(sceGxmShaderPatcherCreateVertexProgram(d.patcher, d.vertexId, attributes, 5, &stream, 1, &d.vertexProgram), "create vertex program")) return false;
    const char *uniforms[] = {"factor", "operations", "arguments", "alphaTest"};
    for (unsigned i = 0; i < 4; ++i) {
        d.params[i] = sceGxmProgramFindParameterByName(fp, uniforms[i]);
        if (!d.params[i]) return d.fail(uniforms[i]);
    }
    // Optional: a package that ships shaders/proven_*.gxp can draw with them.
    for (auto &p : d.proven) {
        FILE *probe = fopen((std::string(directory) + "/" + p.file).c_str(), "rb"); if (!probe) continue; fclose(probe);
        if (!d.load(std::string(directory) + "/" + p.file, p.bytes)) return false;
        auto *program = reinterpret_cast<const SceGxmProgram *>(p.bytes.data());
        if (!d.check(sceGxmShaderPatcherRegisterProgram(d.patcher, program, &p.id), p.file)) return false;
        if (p.uniform && !(p.tint = sceGxmProgramFindParameterByName(program, p.uniform))) return d.fail(p.uniform);
    }
    d.fence = sceGxmGetNotificationRegion() + 64; *d.fence = 0;
    for (auto &r : d.rings) {
        r.size = 4u << 20; void *base = nullptr;
        r.uid = sceKernelAllocMemBlock("touhou-vertices", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, r.size, nullptr);
        if (r.uid < 0 || sceKernelGetMemBlockBase(r.uid, &base) < 0 || sceGxmMapMemory(base, r.size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) return d.fail("vertex ring", r.uid);
        r.base = static_cast<uint8_t *>(base);
    }
    d.white = createTexture(8, 8); if (!d.white) return false;
    std::array<uint32_t,64> white; white.fill(0xffffffff);
    return upload(d.white, white.data(), 32);
}
const char *Device::error() const { return impl->failure.c_str(); }
Device::Statistics Device::statistics() const { return impl->statistics; }
vita2d_texture *Device::createTexture(uint32_t width, uint32_t height, bool renderTarget) {
    if (!width || !height || width > 4096 || height > 4096) { impl->fail("invalid texture dimensions"); return nullptr; }
    if (!renderTarget) for (size_t i = 0; i < impl->textureCache.size(); ++i) {
        const auto c = impl->textureCache[i];
        if (c.width != width || c.height != height || !impl->idle(c.frame)) continue;
        impl->textureCache.erase(impl->textureCache.begin() + i); impl->cacheBytes -= c.bytes;
        memset(vita2d_texture_get_datap(c.texture), 0, c.bytes);   // a new texture reads as zero
        return c.texture;
    }
    // libvita2d's factory leaves optional handles uninitialized on non-RT and
    // failure paths. Own construction, retain its well-defined mapped cleanup.
    auto *t=static_cast<vita2d_texture *>(calloc(1,sizeof(vita2d_texture)));
    if (!t) { impl->fail("texture metadata allocation failed"); return nullptr; }
    auto allocate=[&](uint32_t bytes, bool cdram, SceUID &uid) -> void * { return impl->allocateBlock(bytes,cdram,uid,true); };
    const uint32_t pitch=((width+7)&~7u)*4;
    void *pixels=allocate(pitch*height,true,t->data_UID);
    if (!pixels) pixels=allocate(pitch*height,false,t->data_UID);
    if (!pixels) { free(t); impl->fail("texture memory allocation failed"); return nullptr; }
    auto failed=[&]() -> vita2d_texture * { vita2d_free_texture(t); return nullptr; };
    if (!impl->check(sceGxmTextureInitLinear(&t->gxm_tex,pixels,SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,width,height,0),"initialize texture")) return failed();
    if (renderTarget) {
        if (!impl->check(sceGxmColorSurfaceInit(&t->gxm_sfc,SCE_GXM_COLOR_FORMAT_A8B8G8R8,
            SCE_GXM_COLOR_SURFACE_LINEAR,SCE_GXM_COLOR_SURFACE_SCALE_NONE,SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
            width,height,pitch/4,pixels),"initialize target stride")) return failed();
        const uint32_t depthWidth=(width+SCE_GXM_TILE_SIZEX-1)&~(SCE_GXM_TILE_SIZEX-1);
        const uint32_t depthHeight=(height+SCE_GXM_TILE_SIZEY-1)&~(SCE_GXM_TILE_SIZEY-1);
        // Depth in VRAM (CDRAM) like the colour: it is stored and loaded at every scene split.
        void *depth=allocate(depthWidth*depthHeight*4,true,t->depth_UID);
        if (!depth) depth=allocate(depthWidth*depthHeight*4,false,t->depth_UID);
        if (!depth) { impl->fail("depth allocation failed"); return failed(); }
        if (!impl->check(sceGxmDepthStencilSurfaceInit(&t->gxm_sfd,SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
            SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,depthWidth,depth,nullptr),"initialize depth")) return failed();
        sceGxmDepthStencilSurfaceSetForceStoreMode(&t->gxm_sfd,SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        SceGxmRenderTargetParams target{};
        target.width=width;target.height=height;target.scenesPerFrame=1;
        target.multisampleMode=SCE_GXM_MULTISAMPLE_NONE;target.driverMemBlock=-1;
        if (!impl->check(sceGxmCreateRenderTarget(&target,&t->gxm_rtgt),"create render target")) return failed();
    }
    return t;
}
bool Device::upload(vita2d_texture *t, const void *rgba, uint32_t pitch, int x, int y, int w, int h) {
    if (!t || !rgba) return impl->fail("null texture upload");
    if (w < 0) w = vita2d_texture_get_width(t);
    if (h < 0) h = vita2d_texture_get_height(t);
    if (x < 0 || y < 0 || w < 0 || h < 0 || uint32_t(x+w)>vita2d_texture_get_width(t) || uint32_t(y+h)>vita2d_texture_get_height(t) || pitch < uint32_t(w*4)) return impl->fail("texture upload bounds");
    const auto use = impl->lastUse.find(t);
    if (use != impl->lastUse.end() && !impl->idle(use->second) && !t->gxm_rtgt && x == 0 && y == 0 &&
        uint32_t(w) == vita2d_texture_get_width(t) && uint32_t(h) == vita2d_texture_get_height(t) &&
        impl->replaceStorage(t, use->second))
        impl->lastUse.erase(use);
    impl->waitTexture(t); if (!impl->failure.empty()) return false;
    auto *destination = static_cast<uint8_t *>(vita2d_texture_get_datap(t));
    for (int row = 0; row < h; ++row) memcpy(destination + (y+row)*vita2d_texture_get_stride(t) + x*4, static_cast<const uint8_t *>(rgba) + row*pitch, w*4);
    impl->statistics.uploadBytes += uint64_t(w) * h * 4;
    return true;
}
void Device::destroyTexture(vita2d_texture *t) {
    if (!t) return;
    if (impl->target == t) { impl->end(); impl->target = nullptr; }
    if (!t->gxm_rtgt && impl->initialized) {   // kept for reuse; freed when the cache is full
        auto it = impl->lastUse.find(t); const uint32_t frame = it == impl->lastUse.end() ? 0 : it->second;
        if (it != impl->lastUse.end()) impl->lastUse.erase(it);
        const uint32_t bytes = vita2d_texture_get_stride(t) * vita2d_texture_get_height(t);
        impl->textureCache.push_back({t, vita2d_texture_get_width(t), vita2d_texture_get_height(t), bytes, frame});
        impl->cacheBytes += bytes; impl->trimCache(); return;
    }
    impl->waitTexture(t); impl->lastUse.erase(t);
    vita2d_free_texture(t);
}
DepthBuffer *Device::createDepth(uint32_t width,uint32_t height,bool sixteenBit) {
    if (!width || !height || width>4096 || height>4096) { impl->fail("invalid depth dimensions"); return nullptr; }
    auto *d=new DepthBuffer;
    d->width=width;d->height=height;
    const uint32_t stride=(width+SCE_GXM_TILE_SIZEX-1)&~(SCE_GXM_TILE_SIZEX-1);
    const uint32_t rows=(height+SCE_GXM_TILE_SIZEY-1)&~(SCE_GXM_TILE_SIZEY-1);
    // VRAM (CDRAM, 256 KiB blocks) first, main memory if it is full.
    uint32_t size=(stride*rows*(sixteenBit?2:4)+256*1024-1)&~(256*1024-1u);
    d->uid=sceKernelAllocMemBlock("touhou-depth",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,size,nullptr);
    if (d->uid<0) {
        size=(stride*rows*(sixteenBit?2:4)+4095)&~4095u;
        d->uid=sceKernelAllocMemBlock("touhou-depth",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,size,nullptr);
    }
    if (d->uid<0 || sceKernelGetMemBlockBase(d->uid,&d->memory)<0) {
        if(d->uid>0)sceKernelFreeMemBlock(d->uid);
        delete d;impl->fail("shared depth allocation failed");return nullptr;
    }
    if (!impl->check(sceGxmMapMemory(d->memory,size,static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ|SCE_GXM_MEMORY_ATTRIB_WRITE)),"map shared depth")) {
        sceKernelFreeMemBlock(d->uid);delete d;return nullptr;
    }
    memset(d->memory,0,size);
    const auto format=sixteenBit ? SCE_GXM_DEPTH_STENCIL_FORMAT_D16 : SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24;
    if (!impl->check(sceGxmDepthStencilSurfaceInit(&d->surface,format,SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,stride,d->memory,nullptr),"initialize shared depth")) {
        sceGxmUnmapMemory(d->memory);sceKernelFreeMemBlock(d->uid);delete d;return nullptr;
    }
    sceGxmDepthStencilSurfaceSetForceStoreMode(&d->surface,SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    return d;
}
void Device::destroyDepth(DepthBuffer *d) {
    if (!d)return;
    finish();if(impl->depth==d)impl->depth=nullptr;
    sceGxmUnmapMemory(d->memory);sceKernelFreeMemBlock(d->uid);delete d;
}
bool Device::target(vita2d_texture *t,DepthBuffer *depth) {
    if (t && !t->gxm_rtgt) return impl->fail("texture is not a render target");
    if (depth && (!t || depth->width<vita2d_texture_get_width(t) || depth->height<vita2d_texture_get_height(t))) return impl->fail("shared depth target size mismatch");
    if (impl->target != t || impl->depth != depth) { impl->end(); impl->target = t; impl->depth=depth; ++impl->statistics.targetSwitches; }
    return impl->failure.empty();
}
void Device::viewport(float x, float y, float width, float height, float minDepth, float maxDepth) {
    const float values[] = {x,y,width,height,minDepth,maxDepth}; memcpy(impl->viewport, values, sizeof(values));
    if (impl->scene) impl->applyViewport();
}
const Vertex *Device::stage(const Vertex *vertices, uint32_t count) {
    auto &d = *impl;
    if (!vertices || !count || count > 65535) return nullptr;
    const uint64_t copyStartUs = sceKernelGetProcessTimeWide();
    auto *copy = static_cast<Vertex *>(d.transient(count * sizeof(Vertex)));
    if (!copy) return nullptr;
    memcpy(copy, vertices, count * sizeof(Vertex));
    const uint64_t spent = sceKernelGetProcessTimeWide() - copyStartUs;
    d.statistics.copyUs += spent; d.statistics.drawUs += spent;
    return copy;
}
Vertex *Device::reserve(uint32_t count) {
    auto &d = *impl;
    if (!count || count > 65535) return nullptr;
    auto *space = static_cast<Vertex *>(d.transient(count * sizeof(Vertex)));
    if (!space) { if (!finish()) return nullptr; d.rings[d.ring].used = 0; space = static_cast<Vertex *>(d.transient(count * sizeof(Vertex))); }
    return space;
}
bool Device::draw(const Vertex *vertices, uint32_t count, vita2d_texture *texture, const DrawState &state) {
    auto &d = *impl;
    if (!count) return true;
    if (!vertices || count > 65535) return d.fail("draw count exceeds 16-bit stream");
    const Vertex *copy = stage(vertices, count);
    if (!copy) { if (!finish()) return false; d.rings[d.ring].used = 0; copy = stage(vertices, count); }
    if (!copy) return d.fail("transient draw allocation failed");
    return drawStaged(copy, count, texture, state);
}
bool Device::drawStaged(const Vertex *copy, uint32_t count, vita2d_texture *texture, const DrawState &state) {
    auto &d = *impl;
    if (!count) return true;
    const uint64_t drawStartUs = sceKernelGetProcessTimeWide();
    struct Accumulate { Impl &d; uint64_t start; ~Accumulate() { d.statistics.drawUs += sceKernelGetProcessTimeWide() - start; } } accumulate{d, drawStartUs};
    auto *program = d.fragment(state); if (!program) return false;
    d.statistics.programUs += sceKernelGetProcessTimeWide() - drawStartUs;
    if (!d.begin()) return false;
    auto &b = d.bound;
    if (!b.programsAndModes) {
        sceGxmSetVertexProgram(d.ctx, d.vertexProgram);
        sceGxmSetTwoSidedEnable(d.ctx, SCE_GXM_TWO_SIDED_ENABLED);
        sceGxmSetFrontPolygonMode(d.ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
        sceGxmSetBackPolygonMode(d.ctx, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
        b.programsAndModes = true;
    }
    if (b.fragment != program) { sceGxmSetFragmentProgram(d.ctx, program); b.fragment = program; }
    if (b.cull != int(state.cull)) { sceGxmSetCullMode(d.ctx, state.cull); b.cull = int(state.cull); }
    if (b.depthFunc != int(state.depthFunc)) {
        sceGxmSetFrontDepthFunc(d.ctx, state.depthFunc); sceGxmSetBackDepthFunc(d.ctx, state.depthFunc);
        b.depthFunc = int(state.depthFunc);
    }
    const auto write = state.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    if (b.depthWrite != int(write)) {
        sceGxmSetFrontDepthWriteEnable(d.ctx, write); sceGxmSetBackDepthWriteEnable(d.ctx, write);
        b.depthWrite = int(write);
    }
    auto *bound = texture ? texture : d.white;
    const auto address = [](int mode) { return mode == 0 ? SCE_GXM_TEXTURE_ADDR_REPEAT : mode == 1 ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_CLAMP; };
    sceGxmTextureSetUAddrMode(&bound->gxm_tex, address(state.repeatU ? 0 : state.addressU));
    sceGxmTextureSetVAddrMode(&bound->gxm_tex, address(state.repeatV ? 0 : state.addressV));
    vita2d_texture_set_filters(bound, state.linear && state.linearMin ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT,
                              state.linear && state.linearMag ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    // The texture words (address, format, sampler state) as they will be bound: the same words
    // as the previous draw in this scene need no new binding.
    if (!b.textureSet || memcmp(&b.texture, &bound->gxm_tex, sizeof(SceGxmTexture)) != 0) {
        if (!d.check(sceGxmSetFragmentTexture(d.ctx, 0, &bound->gxm_tex), "bind texture")) return false;
        b.texture = bound->gxm_tex; b.textureSet = true;
    }
    void *buffer = nullptr;
    if (state.program == DrawState::Program::Combine) {
        if (!d.check(sceGxmReserveFragmentDefaultUniformBuffer(d.ctx, &buffer), "reserve uniforms")) return false;
        const float *values[] = {state.factor, state.operations, state.arguments, state.alphaTest};
        for (unsigned i = 0; i < 4; ++i) if (!d.check(sceGxmSetUniformDataF(buffer,d.params[i],0,4,values[i]), "set uniforms")) return false;
    } else if (const auto *tint = d.proven[unsigned(state.program) - 1].tint) {
        if (!d.check(sceGxmReserveFragmentDefaultUniformBuffer(d.ctx, &buffer), "reserve uniforms")) return false;
        if (!d.check(sceGxmSetUniformDataF(buffer, tint, 0, 4, state.tint), "set tint")) return false;
    }
    if (!d.check(sceGxmSetVertexStream(d.ctx, 0, copy), "vertex stream")) return false;
    d.lastUse[bound] = d.frameSerial; if (d.target) d.lastUse[d.target] = d.frameSerial;
    d.submitted = true; ++d.statistics.draws;
    return d.check(sceGxmDraw(d.ctx, state.primitive, SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(), count), "draw");
}
bool Device::clear(bool color, bool depth, const float rgba[4], float z, int x, int y, int w, int h) {
    if (!color && !depth) return true;
    if (w < 0) w = targetWidth();
    if (h < 0) h = targetHeight();
    std::array<float,6> saved; std::copy(impl->viewport, impl->viewport+6, saved.begin());
    viewport(x,y,w,h);
    Vertex vertices[4]{};
    const float xy[4][2] = {{-1,1},{-1,-1},{1,1},{1,-1}};
    for (int i = 0; i < 4; ++i) { vertices[i].position[0]=xy[i][0]; vertices[i].position[1]=xy[i][1]; vertices[i].position[2]=z; vertices[i].position[3]=1; std::copy(rgba,rgba+4,vertices[i].diffuse); vertices[i].fog[3]=1; }
    DrawState state; state.primitive=SCE_GXM_PRIMITIVE_TRIANGLE_STRIP; state.blend=false; state.depthWrite=depth;
    state.colorMask=color ? SCE_GXM_COLOR_MASK_ALL : 0;
    state.operations[0]=state.operations[1]=1; state.operations[2]=state.arguments[0]=0;
    if (impl->proven[1].id) state.program=DrawState::Program::Diffuse;   // the clear color is in the vertices
    const bool ok=draw(vertices,4,nullptr,state);
    viewport(saved[0],saved[1],saved[2],saved[3],saved[4],saved[5]); return ok;
}
bool Device::finish() {
    if (!impl->initialized) return false;
    impl->finishAll();
    return impl->failure.empty();
}
bool Device::present() {
    if (impl->target) return impl->fail("present requires display target");
    auto &d = *impl;
    if (!d.scene && !d.begin()) return false;   // the display scene carries the frame's fence
    SceGxmNotification done{d.fence, d.frameSerial};
    if (!d.check(sceGxmEndScene(d.ctx, nullptr, &done), "end display scene")) return false;
    d.scene = false;
    vita2d_swap_buffers();
    ++d.frameSerial; ++d.statistics.frames;
    // The other ring was filled two frames ago: reuse it once that frame is done.
    d.ring ^= 1; auto &next = d.rings[d.ring]; d.waitFrame(next.frame); next.used = 0; next.frame = 0;
    return d.failure.empty();
}
bool Device::read(vita2d_texture *t, void *rgba, uint32_t pitch) {
    if (!rgba || !finish()) return false;
    ++impl->statistics.readbacks;
    const uint32_t width=t ? vita2d_texture_get_width(t) : 960, height=t ? vita2d_texture_get_height(t) : 544;
    if (pitch < width*4) return impl->fail("readback stride");
    const auto *src=static_cast<const uint8_t *>(t ? vita2d_texture_get_datap(t) : vita2d_get_current_fb());
    const uint32_t sourcePitch=t ? vita2d_texture_get_stride(t) : 960*4;
    for (uint32_t y=0;y<height;++y) memcpy(static_cast<uint8_t *>(rgba)+y*pitch,src+y*sourcePitch,width*4);
    return true;
}
uint32_t Device::targetWidth() const { return impl->target ? vita2d_texture_get_width(impl->target) : 960; }
uint32_t Device::targetHeight() const { return impl->target ? vita2d_texture_get_height(impl->target) : 544; }
}
