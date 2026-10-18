#pragma once

// Original TH07 resources are little endian. Decode numeric fields once, at
// their loading boundary, so the native script interpreters can retain their
// pointers and mutable variables. Strings, byte flags and 24-bit pixels are
// deliberately not treated as machine words. All offsets are checked first.
#include "ZunEndian.hpp"
#include <algorithm>
#include <vector>

namespace GameDataEndian
{
using namespace ZunEndian;
using Byte = uint8_t;
inline bool Fits(size_t offset, size_t bytes, size_t size)
{
    return offset <= size && bytes <= size - offset;
}
inline void Words(Byte *p, size_t bytes, bool be)
{
    for (size_t i = 0; i < bytes; i += 4) ToNative32(p + i, be);
}
inline void Halves(Byte *p, size_t bytes, bool be)
{
    for (size_t i = 0; i < bytes; i += 2) ToNative16(p + i, be);
}
inline void Unique(std::vector<size_t> &offsets)
{
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
}

inline bool Anm(void *data, size_t size, bool be = kBigEndian)
{
    auto *file = static_cast<Byte *>(data);
    for (size_t base = 0; ; )
    {
        if (!Fits(base, 0x40, size)) return false;
        Byte *p = file + base;
        const size_t sprites = Read32(p), scripts = Read32(p + 4);
        const size_t next = Read32(p + 0x38), texture = Read32(p + 0x30);
        const size_t span = next ? next : size - base;
        if (span < 0x40 || !Fits(base, span, size) || Read32(p + 0x28) != 2 ||
            sprites > (span - 0x40) / 4 || scripts > (span - 0x40 - sprites * 4) / 8)
            return false;
        const size_t tableEnd = 0x40 + sprites * 4 + scripts * 8;
        const size_t name = Read32(p + 0x1c), mipmap = Read32(p + 0x24);
        if (name < tableEnd || name >= span || !std::memchr(p + name, 0, span - name) ||
            (mipmap && (mipmap < tableEnd || mipmap >= span ||
                        !std::memchr(p + mipmap, 0, span - mipmap)))) return false;
        std::vector<size_t> spriteOffsets, scriptOffsets;
        for (size_t i = 0; i < sprites; ++i)
        {
            const size_t offset = Read32(p + 0x40 + i * 4);
            if (offset < tableEnd || !Fits(offset, 20, span) || offset % 4) return false;
            spriteOffsets.push_back(offset);
        }
        for (size_t i = 0; i < scripts; ++i)
        {
            const size_t offset = Read32(p + 0x40 + sprites * 4 + i * 8 + 4);
            if (offset < tableEnd || !Fits(offset, 8, span) || offset % 4) return false;
            scriptOffsets.push_back(offset);
        }
        Unique(spriteOffsets);
        Unique(scriptOffsets);
        // Scripts terminate with opcode -1, while EXIT/STOP may be followed
        // by interrupt handlers and must not end the conversion walk.
        for (size_t i = 0; i < scriptOffsets.size(); ++i)
        {
            size_t end = i + 1 < scriptOffsets.size() ? scriptOffsets[i + 1] : span;
            if (texture && texture < end && texture > scriptOffsets[i]) end = texture;
            size_t cursor = scriptOffsets[i];
            bool terminated = false;
            while (Fits(cursor, 8, end))
            {
                Byte *instr = p + cursor;
                const uint16_t opcode = Read16(instr), bytes = Read16(instr + 2);
                // The physical -1 marker has a zero length in original ANMs.
                if (opcode == 0xffff)
                {
                    Halves(instr, 8, be); terminated = true; break;
                }
                if (bytes < 8 || bytes % 4 || !Fits(cursor, bytes, end)) return false;
                Halves(instr, 8, be);
                Words(instr + 8, bytes - 8, be);
                cursor += bytes;
            }
            if (!terminated) return false;
        }
        for (size_t offset : spriteOffsets) Words(p + offset, 20, be);
        if (p[0x34])
        {
            if (texture < tableEnd || !Fits(texture, 16, span)) return false;
            Byte *image = p + texture;
            const size_t format = Read16(image + 6), width = Read16(image + 8), height = Read16(image + 10);
            const size_t bpp = format == 1 ? 4 : format == 4 ? 3 : 2;
            if (format < 1 || format > 5 || !width || !height ||
                !Fits(texture + 16, width * height * bpp, span)) return false;
            Halves(image, 12, be); ToNative32(image + 12, be);
            if (bpp == 4) Words(image + 16, width * height * bpp, be);
            if (bpp == 2) Halves(image + 16, width * height * bpp, be);
        }
        Words(p, 0x34, be);
        ToNative16(p + 0x36, be);
        Words(p + 0x38, tableEnd - 0x38, be);
        if (!next) return true;
        base += next;
    }
}

inline bool Ecl(void *data, size_t size, bool be = kBigEndian)
{
    auto *p = static_cast<Byte *>(data);
    if (size < 0x44) return false;
    const size_t subs = Read16(p), timelines = Read16(p + 2);
    if (timelines > 16 || subs > (size - 0x44) / 4) return false;
    const size_t header = 0x44 + subs * 4;
    std::vector<size_t> boundaries, subOffsets, timelineOffsets;
    for (size_t i = 0; i < subs; ++i) subOffsets.push_back(Read32(p + 0x44 + i * 4));
    for (size_t i = 0; i < timelines; ++i) timelineOffsets.push_back(Read32(p + 4 + i * 4));
    boundaries = subOffsets;
    boundaries.insert(boundaries.end(), timelineOffsets.begin(), timelineOffsets.end());
    for (size_t offset : boundaries)
        if (offset < header || offset >= size || offset % 4) return false;
    boundaries.push_back(size); Unique(boundaries); Unique(subOffsets); Unique(timelineOffsets);
    for (size_t start : subOffsets)
    {
        const size_t end = *std::upper_bound(boundaries.begin(), boundaries.end(), start);
        size_t cursor = start;
        while (cursor < end)
        {
            if (!Fits(cursor, 12, end)) return false;
            Byte *instr = p + cursor;
            const uint16_t opcode = Read16(instr + 4), bytes = Read16(instr + 6);
            if (bytes < 12 || bytes % 4 || !Fits(cursor, bytes, end)) return false;
            ToNative32(instr, be); Halves(instr + 4, 4, be); ToNative16(instr + 10, be);
            // Spellcard opcode 90 has one numeric argument followed by a
            // 48-byte XOR-encrypted Shift-JIS name.
            if (opcode == 90)
            {
                if (bytes != 64) return false;
                ToNative32(instr + 12, be);
            }
            else Words(instr + 12, bytes - 12, be);
            cursor += bytes;
        }
    }
    for (size_t start : timelineOffsets)
    {
        const size_t end = *std::upper_bound(boundaries.begin(), boundaries.end(), start);
        size_t cursor = start;
        bool terminated = false;
        while (Fits(cursor, 4, end))
        {
            Byte *instr = p + cursor;
            const uint16_t time = Read16(instr);
            // A timeline may end with only the four-byte time/arg0 sentinel.
            if (time == 0xffff) { Halves(instr, 4, be); terminated = true; break; }
            if (!Fits(cursor, 8, end)) return false;
            const uint16_t bytes = Read16(instr + 6);
            if (bytes < 8 || bytes % 4 || !Fits(cursor, bytes, end)) return false;
            Halves(instr, 8, be); Words(instr + 8, bytes - 8, be);
            cursor += bytes;
        }
        if (!terminated) return false;
    }
    Halves(p, 4, be); Words(p + 4, header - 4, be);
    return true;
}

inline bool Msg(void *data, size_t size, bool be = kBigEndian)
{
    auto *p = static_cast<Byte *>(data);
    if (size < 4) return false;
    const size_t count = Read32(p);
    if (count > (size - 4) / 4) return false;
    const size_t header = 4 + count * 4;
    std::vector<size_t> offsets;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = Read32(p + 4 + i * 4);
        if (offset < header || offset > size) return false;
        if (offset < size) offsets.push_back(offset);
    }
    Unique(offsets); offsets.push_back(size);
    for (size_t i = 0; i + 1 < offsets.size(); ++i)
    {
        size_t cursor = offsets[i], end = offsets[i + 1];
        bool terminated = false;
        while (Fits(cursor, 4, end))
        {
            Byte *instr = p + cursor;
            const unsigned opcode = instr[2], bytes = instr[3];
            if (!Fits(cursor + 4, bytes, end)) return false;
            ToNative16(instr, be);
            switch (opcode)
            {
            case 1: case 2: case 3: case 8:
                if (bytes < 4) return false;
                Halves(instr + 4, 4, be); break;
            case 4: case 7:
                if (bytes < 4) return false;
                ToNative32(instr + 4, be); break;
            case 5:
                if (bytes < 3) return false;
                ToNative16(instr + 4, be); break;
            default: break;
            }
            cursor += 4 + bytes;
            if (opcode == 0) { terminated = true; break; }
        }
        if (!terminated) return false;
    }
    Words(p, header, be);
    return true;
}

inline bool Sht(void *data, size_t size, bool be = kBigEndian)
{
    auto *p = static_cast<Byte *>(data);
    if (size < 0x34) return false;
    const size_t count = Read16(p + 2);
    if (count > (size - 0x34) / 8) return false;
    const size_t header = 0x34 + count * 8;
    std::vector<size_t> entries;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = Read32(p + 0x34 + i * 8);
        if (offset < header || offset >= size || offset % 4) return false;
        entries.push_back(offset);
    }
    Unique(entries); entries.push_back(size);
    for (size_t i = 0; i + 1 < entries.size(); ++i)
    {
        size_t cursor = entries[i]; bool terminated = false;
        while (Fits(cursor, 2, entries[i + 1]))
        {
            Byte *entry = p + cursor;
            if (Read16(entry) == 0xffff) { ToNative16(entry, be); terminated = true; break; }
            if (!Fits(cursor, 0x34, entries[i + 1])) return false;
            Halves(entry, 4, be); Words(entry + 4, 24, be);
            ToNative16(entry + 28, be); Halves(entry + 32, 4, be); Words(entry + 36, 16, be);
            cursor += 0x34;
        }
        if (!terminated) return false;
    }
    Halves(p, 4, be); Words(p + 4, header - 4, be);
    return true;
}

inline bool Std(void *data, size_t size, bool be = kBigEndian)
{
    auto *p = static_cast<Byte *>(data);
    if (size < 0x490) return false;
    const size_t count = Read16(p), instances = Read32(p + 4), script = Read32(p + 8);
    if (count > (size - 0x490) / 4) return false;
    const size_t header = 0x490 + count * 4;
    if (instances < header || script < header || instances >= script || script >= size ||
        instances % 4 || script % 4) return false;
    std::vector<size_t> objects;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = Read32(p + 0x490 + i * 4);
        if (offset < header || !Fits(offset, 28, instances) || offset % 4) return false;
        objects.push_back(offset);
    }
    Unique(objects); objects.push_back(instances);
    for (size_t i = 0; i + 1 < objects.size(); ++i)
    {
        Byte *object = p + objects[i];
        ToNative16(object, be); Words(object + 4, 24, be);
        size_t cursor = objects[i] + 28; bool terminated = false;
        while (Fits(cursor, 4, objects[i + 1]))
        {
            Byte *quad = p + cursor;
            if (Read16(quad) == 0xffff) { Halves(quad, 4, be); terminated = true; break; }
            const size_t bytes = Read16(quad + 2);
            if (bytes < 28 || bytes % 4 || !Fits(cursor, bytes, objects[i + 1])) return false;
            Halves(quad, 8, be); Words(quad + 8, bytes - 8, be);
            cursor += bytes;
        }
        if (!terminated) return false;
    }
    size_t cursor = instances; bool terminated = false;
    while (Fits(cursor, 16, script))
    {
        const bool last = Read16(p + cursor) == 0xffff;
        Halves(p + cursor, 4, be); Words(p + cursor + 4, 12, be); cursor += 16;
        if (last) { terminated = true; break; }
    }
    if (!terminated) return false;
    // STD's size field denotes argument bytes (12), and the interpreter
    // advances fixed 20-byte records, unlike ECL and ANM.
    for (cursor = script; Fits(cursor, 20, size); cursor += 20)
    {
        const bool last = Read32(p + cursor) == 0xffffffffu;
        ToNative32(p + cursor, be); Halves(p + cursor + 4, 4, be); Words(p + cursor + 8, 12, be);
        if (last) { Halves(p, 4, be); Words(p + 4, 12, be); Words(p + 0x490, count * 4, be); return true; }
    }
    return false;
}
} // namespace GameDataEndian
