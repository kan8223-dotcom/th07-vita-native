// TH07 Vita: TTF_OpenFont (linked with -Wl,--wrap=TTF_OpenFont) opens the font from memory.
// SDL_ttf otherwise reads the 4.4 MB NotoSansJP-Regular.ttf through stdio, a seek and small
// read on ux0 for every glyph FreeType loads (the TH06 Vita port: dialogue lines hitched).
// The file is read once per path and kept for the process; the bytes are what the file
// holds, so the rendered text is unchanged. TextHelper's own fallback order (data folder,
// then app0:) still applies: a path that cannot be read is opened by the original call.
#include <SDL2/SDL_rwops.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Platform.hpp"

extern "C" TTF_Font *__real_TTF_OpenFont(const char *file, int ptsize);

namespace
{
struct FontBytes
{
    std::string path;
    std::vector<unsigned char> bytes;
};
std::vector<FontBytes> g_Fonts;

const FontBytes *LoadFont(const char *file)
{
    for (const auto &font : g_Fonts)
        if (font.path == file)
            return &font;
    FILE *in = std::fopen(file, "rb");   // the game's fopen: relative paths resolve to ux0:data/th07
    if (!in)
        return nullptr;
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), in)) > 0)
        bytes.insert(bytes.end(), chunk, chunk + got);
    const bool failed = std::ferror(in) != 0;
    std::fclose(in);
    if (failed || bytes.empty())
        return nullptr;
    g_Fonts.push_back({file, std::move(bytes)});
    th07_vita_log("FONT %s read into memory bytes=%u", file, unsigned(g_Fonts.back().bytes.size()));
    return &g_Fonts.back();
}
} // namespace

extern "C" TTF_Font *__wrap_TTF_OpenFont(const char *file, int ptsize)
{
    const FontBytes *font = file ? LoadFont(file) : nullptr;
    if (!font)
        return __real_TTF_OpenFont(file, ptsize);
    SDL_RWops *stream = SDL_RWFromConstMem(font->bytes.data(), int(font->bytes.size()));
    if (!stream)
        return __real_TTF_OpenFont(file, ptsize);
    return TTF_OpenFontRW(stream, 1, ptsize);   // frees the RWops (not the bytes) on TTF_CloseFont
}
