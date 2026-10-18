// TH07: AnmManager::CopySurfaceToBackBuffer / DrawEndingRect (startup logo, title, menus,
// result screen, music room, ending), linked with -Wl,--wrap for both (and for
// LoadSurface/ReleaseSurface, which count a generation per surface slot).
//
// The inherited non-PSP path makes a texture of the whole 640x480 surface, uploads it, draws
// one quad and deletes the texture, every frame. On the Vita that is a 1.2 MB conversion and
// upload per frame plus a GPU wait at the delete (the TH06 Vita title ran at 19 fps from
// exactly this). Here each slot keeps its texture, made again only when the slot's surface
// changed (a LoadSurface/ReleaseSurface of the slot; all their callers are in other
// translation units, LoadSurface's own ReleaseSurface call included in its generation).
//
// The quad, its texture coordinates and every render-state call are the inherited ones.
// Afterwards the texture the AnmManager believes bound is bound again: the inherited path
// left none bound (it deleted its texture), while the original's surface copy changed no
// texture state, so a following sprite of the same atlas (no rebind) drew untextured.
// Engine-neutral: the TH07 PC reference build (~/th07_hostref_20261013) links this file too.
#include "AnmManager.hpp"
#include "Supervisor.hpp"
#include "graphics/ZunGraphics.hpp"
#include <SDL2/SDL_surface.h>

namespace
{
struct CachedSurface
{
    SDL_Surface *surface = nullptr;
    GfxTextureHandle handle{0};
    int width = 0, height = 0;
    unsigned generation = 0;
};
CachedSurface g_Cache[32];
unsigned g_Generation[32];

GfxTextureHandle SurfaceTexture(int slot, SDL_Surface *surface)
{
    CachedSurface &entry = g_Cache[slot];
    ZunGraphics *gfx = g_Supervisor.gfxDevice;
    if (entry.handle.id && entry.surface == surface && entry.width == surface->w && entry.height == surface->h &&
        entry.generation == g_Generation[slot])
    {
        gfx->BindTexture(entry.handle);
        return entry.handle;
    }
    if (entry.handle.id)
        gfx->DeleteTexture(entry.handle);
    entry.handle = gfx->CreateTexture();
    gfx->BindTexture(entry.handle);
    gfx->SetTextureImage(surface->w, surface->h, PIXEL_RGBA, PIXEL_UNSIGNED_BYTE, surface->pixels);
    entry.surface = surface;
    entry.width = surface->w;
    entry.height = surface->h;
    entry.generation = g_Generation[slot];
    return entry.handle;
}

void RestoreBinding(AnmManager *manager)
{
    g_Supervisor.gfxDevice->BindTexture(manager->currentTexture);
}
} // namespace

extern "C" void __wrap__ZN10AnmManager23CopySurfaceToBackBufferEiiiii(AnmManager *manager, i32 surfaceIdx,
                                                                    i32 left, i32 top, i32 x, i32 y)
{
    if (surfaceIdx < 0 || surfaceIdx >= 32 || !manager->surfacesBis[surfaceIdx])
    {
        return;
    }
    SDL_Surface *surf = manager->surfacesBis[surfaceIdx];
    SurfaceTexture(surfaceIdx, surf);

    VertexTex1DiffuseXyzrhw vertices[4];
    f32 width = (f32)manager->surfaceSourceInfo[surfaceIdx].width;
    f32 height = (f32)manager->surfaceSourceInfo[surfaceIdx].height;

    vertices[0].pos = ZunVec3(x, y, 0.0f);
    vertices[1].pos = ZunVec3(x + width, y, 0.0f);
    vertices[2].pos = ZunVec3(x, y + height, 0.0f);
    vertices[3].pos = ZunVec3(x + width, y + height, 0.0f);
    vertices[0].w = vertices[1].w = vertices[2].w = vertices[3].w = 1.0f;

    f32 u0 = (f32)left / surf->w;
    f32 v0 = (f32)top / surf->h;
    f32 u1 = (f32)manager->surfaceSourceInfo[surfaceIdx].width / surf->w;
    f32 v1 = (f32)manager->surfaceSourceInfo[surfaceIdx].height / surf->h;

    vertices[0].textureUV = {u0, v0};
    vertices[1].textureUV = {u1, v0};
    vertices[2].textureUV = {u0, v1};
    vertices[3].textureUV = {u1, v1};

    vertices[0].color.color = vertices[1].color.color = vertices[2].color.color = vertices[3].color.color =
        0xFFFFFFFF;

    g_Supervisor.gfxDevice->SetDepthMask(false);
    g_Supervisor.gfxDevice->SetBlendMode(BLEND_NONE, BLEND_NONE);
    g_Supervisor.gfxDevice->SetColorOp(COMPONENT_RGB, COLOR_OP_MODULATE);
    g_Supervisor.gfxDevice->SetColorOp(COMPONENT_ALPHA, COLOR_OP_MODULATE);
    g_Supervisor.gfxDevice->Disable(CAPS_ALPHA_TEST);
    g_Supervisor.gfxDevice->DrawPrimitiveUP(PRIM_TRIANGLE_STRIP, 2, vertices, sizeof(VertexTex1DiffuseXyzrhw));
    g_Supervisor.gfxDevice->Enable(CAPS_ALPHA_TEST);
    RestoreBinding(manager);
}

extern "C" void __wrap__ZN10AnmManager14DrawEndingRectEiiiiiii(AnmManager *manager, i32 surfaceIdx, i32 rectX,
                                                              i32 rectY, i32 rectLeft, i32 rectTop, i32 width,
                                                              i32 height)
{
    if (surfaceIdx < 0 || surfaceIdx >= 32 || !manager->surfacesBis[surfaceIdx])
    {
        return;
    }
    SDL_Surface *surf =
        manager->surfaces[surfaceIdx] ? manager->surfaces[surfaceIdx] : manager->surfacesBis[surfaceIdx];
    SurfaceTexture(surfaceIdx, surf);

    VertexTex1DiffuseXyzrhw vertices[4];
    f32 drawWidth = width;
    f32 drawHeight = height;

    vertices[0].pos = ZunVec3(rectX, rectY, 0.0f);
    vertices[1].pos = ZunVec3(rectX + drawWidth, rectY, 0.0f);
    vertices[2].pos = ZunVec3(rectX, rectY + drawHeight, 0.0f);
    vertices[3].pos = ZunVec3(rectX + drawWidth, rectY + drawHeight, 0.0f);
    vertices[0].w = vertices[1].w = vertices[2].w = vertices[3].w = 1.0f;

    f32 u0 = (f32)rectLeft / surf->w;
    f32 v0 = (f32)rectTop / surf->h;
    f32 u1 = (f32)(rectLeft + width) / surf->w;
    f32 v1 = (f32)(rectTop + height) / surf->h;

    vertices[0].textureUV = {u0, v0};
    vertices[1].textureUV = {u1, v0};
    vertices[2].textureUV = {u0, v1};
    vertices[3].textureUV = {u1, v1};

    vertices[0].color.color = vertices[1].color.color = vertices[2].color.color = vertices[3].color.color =
        0xFFFFFFFF;

    g_Supervisor.gfxDevice->SetDepthMask(false);
    g_Supervisor.gfxDevice->SetBlendMode(BLEND_NONE, BLEND_NONE);
    g_Supervisor.gfxDevice->SetColorOp(COMPONENT_RGB, COLOR_OP_MODULATE);
    g_Supervisor.gfxDevice->SetColorOp(COMPONENT_ALPHA, COLOR_OP_MODULATE);
    g_Supervisor.gfxDevice->DrawPrimitiveUP(PRIM_TRIANGLE_STRIP, 2, vertices, sizeof(VertexTex1DiffuseXyzrhw));
    RestoreBinding(manager);
}

extern "C" ZunResult __real__ZN10AnmManager11LoadSurfaceEiPKc(AnmManager *manager, i32 surfaceIdx, const char *path);
extern "C" ZunResult __wrap__ZN10AnmManager11LoadSurfaceEiPKc(AnmManager *manager, i32 surfaceIdx, const char *path)
{
    if (surfaceIdx >= 0 && surfaceIdx < 32)
    {
        ++g_Generation[surfaceIdx];
    }
    return __real__ZN10AnmManager11LoadSurfaceEiPKc(manager, surfaceIdx, path);
}
extern "C" void __real__ZN10AnmManager14ReleaseSurfaceEi(AnmManager *manager, i32 surfaceIdx);
extern "C" void __wrap__ZN10AnmManager14ReleaseSurfaceEi(AnmManager *manager, i32 surfaceIdx)
{
    if (surfaceIdx >= 0 && surfaceIdx < 32)
    {
        ++g_Generation[surfaceIdx];
        // The surface is gone: so is its texture (the inherited path never kept one).
        CachedSurface &entry = g_Cache[surfaceIdx];
        if (entry.handle.id && g_Supervisor.gfxDevice)
        {
            g_Supervisor.gfxDevice->DeleteTexture(entry.handle);
        }
        entry = CachedSurface{};
    }
    __real__ZN10AnmManager14ReleaseSurfaceEi(manager, surfaceIdx);
}
