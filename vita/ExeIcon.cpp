#include "ExeIcon.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <zlib.h>

namespace exeicon
{
namespace
{

struct Reader
{
    const uint8_t *data;
    size_t size;
    bool ok = true;
    uint32_t u32(size_t at)
    {
        if (at > size || size - at < 4) { ok = false; return 0; }
        return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 | uint32_t(data[at + 3]) << 24;
    }
    uint16_t u16(size_t at)
    {
        if (at > size || size - at < 2) { ok = false; return 0; }
        return uint16_t(data[at] | data[at + 1] << 8);
    }
    uint8_t u8(size_t at)
    {
        if (at >= size) { ok = false; return 0; }
        return data[at];
    }
};

struct Section { uint32_t address, virtualSize, rawSize, rawOffset; };

struct Leaf
{
    uint32_t type, name, language;
    bool namedName = false; // string names sort after numeric ones
    size_t offset, size;
};

struct Pe
{
    Reader r;
    std::vector<Section> sections;
    size_t resourceOffset = 0;
    uint32_t resourceSize = 0;
    std::vector<Leaf> leaves;
    std::string error;

    bool rva(uint32_t address, uint32_t size, size_t &offset)
    {
        for (const Section &s : sections)
            if (s.address <= address && uint64_t(address) + size <= uint64_t(s.address) + s.rawSize)
            {
                offset = size_t(s.rawOffset) + (address - s.address);
                if (offset > r.size || r.size - offset < size) return false;
                return true;
            }
        return false;
    }
    bool walk(uint32_t offset, int depth, uint32_t keys[3], bool named[3])
    {
        if (depth > 2 || offset > resourceSize || resourceSize - offset < 16) { error = "resource directory"; return false; }
        const size_t base = resourceOffset + offset;
        const unsigned count = unsigned(r.u16(base + 12)) + r.u16(base + 14);
        for (unsigned i = 0; i < count; ++i)
        {
            const uint32_t entry = offset + 16 + i * 8;
            if (entry > resourceSize || resourceSize - entry < 8) { error = "resource entry"; return false; }
            const uint32_t name = r.u32(resourceOffset + entry), child = r.u32(resourceOffset + entry + 4);
            keys[depth] = name & 0x7fffffffu;
            named[depth] = (name & 0x80000000u) != 0;
            if (child & 0x80000000u)
            {
                if (!walk(child & 0x7fffffffu, depth + 1, keys, named)) return false;
            }
            else
            {
                if (depth != 2) { error = "resource leaf depth"; return false; }
                if (child > resourceSize || resourceSize - child < 16) { error = "resource data entry"; return false; }
                const uint32_t dataRva = r.u32(resourceOffset + child), dataSize = r.u32(resourceOffset + child + 4);
                size_t at;
                if (!rva(dataRva, dataSize, at)) { error = "resource data outside sections"; return false; }
                Leaf leaf{keys[0], keys[1], keys[2], named[1], at, dataSize};
                if (named[0]) leaf.type = 0xffffffffu; // string types are never icons
                leaves.push_back(leaf);
            }
        }
        return r.ok;
    }
    bool parse()
    {
        if (r.size < 0x40 || r.data[0] != 'M' || r.data[1] != 'Z') { error = "not an MZ executable"; return false; }
        const uint32_t pe = r.u32(0x3c);
        if (r.u32(pe) != 0x00004550u) { error = "not a PE executable"; return false; }
        const unsigned sectionCount = r.u16(pe + 6), optionalSize = r.u16(pe + 20);
        const size_t optional = pe + 24;
        const uint16_t magic = r.u16(optional);
        size_t directory;
        if (magic == 0x10b) directory = optional + 96;
        else if (magic == 0x20b) directory = optional + 112;
        else { error = "unknown PE optional header"; return false; }
        const uint32_t resourceRva = r.u32(directory + 16);
        resourceSize = r.u32(directory + 20);
        for (unsigned i = 0; i < sectionCount; ++i)
        {
            const size_t s = optional + optionalSize + i * 40;
            sections.push_back({r.u32(s + 12), r.u32(s + 8), r.u32(s + 16), r.u32(s + 20)});
        }
        if (!r.ok) { error = "truncated PE headers"; return false; }
        if (!rva(resourceRva, resourceSize, resourceOffset)) { error = "no resource section"; return false; }
        uint32_t keys[3] = {};
        bool named[3] = {};
        return walk(0, 0, keys, named);
    }
};

// One RT_ICON payload (BITMAPINFOHEADER DIB, XOR image + AND mask) to RGBA,
// as Pillow's ICO reader does for BMP-based entries.
bool DecodeDib(const uint8_t *p, size_t size, unsigned w, unsigned h, unsigned &bpp, std::vector<uint8_t> &rgba,
               std::string &error)
{
    Reader r{p, size};
    if (size >= 8 && std::memcmp(p, "\x89PNG", 4) == 0) { error = "PNG icon entry"; return false; }
    const uint32_t headerSize = r.u32(0);
    const int32_t dibW = int32_t(r.u32(4)), dibH2 = int32_t(r.u32(8));
    bpp = r.u16(14);
    const uint32_t compression = r.u32(16), used = r.u32(32);
    if (!r.ok || headerSize < 40 || compression != 0) { error = "unsupported icon bitmap"; return false; }
    if (unsigned(dibW) != w || unsigned(dibH2) != h * 2) { error = "icon size disagrees with group"; return false; }
    const unsigned colors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    const size_t palette = headerSize;
    const size_t xorOffset = palette + size_t(colors) * 4;
    const size_t xorStride = (size_t(w) * bpp + 31) / 32 * 4;
    const size_t andOffset = xorOffset + xorStride * h;
    const size_t andStride = (size_t(w) + 31) / 32 * 4;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) { error = "unsupported icon depth"; return false; }
    const bool hasMask = bpp != 32;
    if (andOffset + (hasMask ? andStride * h : 0) > size) { error = "truncated icon bitmap"; return false; }
    rgba.assign(size_t(w) * h * 4, 0);
    for (unsigned y = 0; y < h; ++y)
    {
        const uint8_t *row = p + xorOffset + xorStride * (h - 1 - y); // bottom-up
        const uint8_t *mask = p + andOffset + andStride * (h - 1 - y);
        for (unsigned x = 0; x < w; ++x)
        {
            uint8_t *out = &rgba[(size_t(y) * w + x) * 4];
            if (bpp <= 8)
            {
                const unsigned bit = x * bpp;
                const unsigned index = (row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                if (index >= colors) { error = "palette index out of range"; return false; }
                const uint8_t *c = p + palette + index * 4;
                out[0] = c[2]; out[1] = c[1]; out[2] = c[0];
            }
            else
            {
                const uint8_t *c = row + x * (bpp / 8);
                out[0] = c[2]; out[1] = c[1]; out[2] = c[0];
                if (bpp == 32) out[3] = c[3];
            }
            if (hasMask) out[3] = (mask[x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
        }
    }
    return true;
}

void Be32(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t> &png, const char *type, const std::vector<uint8_t> &data)
{
    Be32(png, uint32_t(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    Be32(png, uint32_t(crc32(0, png.data() + start, uInt(png.size() - start))));
}

} // namespace

bool ExtractLargest(const uint8_t *exe, size_t size, Icon &icon, std::string &error)
{
    Pe pe;
    pe.r = Reader{exe, size};
    if (!pe.parse()) { error = pe.error.empty() ? "PE parse failed" : pe.error; return false; }
    std::vector<Icon> candidates;
    for (const Leaf &group : pe.leaves)
    {
        if (group.type != 14) continue; // RT_GROUP_ICON
        Reader g{exe + group.offset, group.size};
        const unsigned reserved = g.u16(0), type = g.u16(2), count = g.u16(4);
        if (!g.ok || reserved != 0 || type != 1 || group.size != 6 + size_t(count) * 14) { error = "invalid RT_GROUP_ICON"; return false; }
        for (unsigned ordinal = 0; ordinal < count; ++ordinal)
        {
            const size_t e = 6 + size_t(ordinal) * 14;
            const unsigned w = g.u8(e) ? g.u8(e) : 256, h = g.u8(e + 1) ? g.u8(e + 1) : 256;
            const uint32_t bytes = g.u32(e + 8);
            const unsigned iconId = g.u16(e + 12);
            const Leaf *found = nullptr;
            unsigned alternatives = 0;
            for (const Leaf &l : pe.leaves)
                if (l.type == 3 && !l.namedName && l.name == iconId)
                {
                    if (l.language == group.language) { found = &l; alternatives = 1; break; }
                    ++alternatives;
                    found = &l;
                }
            if (!found || alternatives != 1) { error = "ambiguous or missing RT_ICON"; return false; }
            if (found->size != bytes) { error = "RT_GROUP_ICON/RT_ICON size mismatch"; return false; }
            Icon c;
            c.width = w; c.height = h;
            c.groupId = group.name; c.languageId = group.language; c.resourceId = iconId; c.ordinal = ordinal;
            std::string why;
            if (!DecodeDib(exe + found->offset, found->size, w, h, c.bpp, c.rgba, why))
                continue; // e.g. PNG-compressed 256px entries: not usable at 128px anyway
            candidates.push_back(std::move(c));
        }
    }
    if (candidates.empty()) { error = "no usable icon in the executable"; return false; }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Icon &a, const Icon &b) {
        if (a.width * a.height != b.width * b.height) return a.width * a.height > b.width * b.height;
        if (a.bpp != b.bpp) return a.bpp > b.bpp;
        if (a.groupId != b.groupId) return a.groupId < b.groupId;
        if (a.languageId != b.languageId) return a.languageId < b.languageId;
        return a.ordinal < b.ordinal;
    });
    icon = std::move(candidates.front());
    return true;
}

bool Enlarge128(const Icon &icon, std::vector<uint8_t> &out, std::string &error)
{
    if (icon.width != icon.height || icon.width == 0 || icon.width > 128 || 128 % icon.width)
    {
        error = "icon cannot fill 128px by integer enlargement";
        return false;
    }
    const unsigned scale = 128 / icon.width;
    out.resize(128 * 128 * 4);
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 128; ++x)
            std::memcpy(&out[(y * 128 + x) * 4], &icon.rgba[((y / scale) * icon.width + x / scale) * 4], 4);
    return true;
}

bool EncodePng(const std::vector<uint8_t> &rgba, unsigned width, unsigned height, std::vector<uint8_t> &png,
               bool &indexed, std::string &error)
{
    if (rgba.size() != size_t(width) * height * 4) { error = "bad image size"; return false; }
    std::map<uint32_t, unsigned> colors;
    std::vector<uint32_t> order;
    std::vector<uint8_t> indices(size_t(width) * height);
    indexed = true;
    for (size_t i = 0; i < indices.size(); ++i)
    {
        uint32_t key;
        std::memcpy(&key, &rgba[i * 4], 4);
        auto it = colors.find(key);
        if (it == colors.end())
        {
            if (order.size() == 256) { indexed = false; break; }
            it = colors.emplace(key, unsigned(order.size())).first;
            order.push_back(key);
        }
        indices[i] = uint8_t(it->second);
    }
    std::vector<uint8_t> raw;
    const unsigned channels = indexed ? 1 : 4;
    raw.reserve((size_t(width) * channels + 1) * height);
    for (unsigned y = 0; y < height; ++y)
    {
        raw.push_back(0); // filter: none
        if (indexed) raw.insert(raw.end(), &indices[size_t(y) * width], &indices[size_t(y) * width] + width);
        else raw.insert(raw.end(), &rgba[size_t(y) * width * 4], &rgba[size_t(y) * width * 4] + width * 4);
    }
    uLongf packedSize = compressBound(uLong(raw.size()));
    std::vector<uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), uLong(raw.size()), 9) != Z_OK) { error = "zlib failed"; return false; }
    packed.resize(packedSize);
    png.assign({0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'});
    std::vector<uint8_t> header;
    Be32(header, width); Be32(header, height);
    header.push_back(8); header.push_back(indexed ? 3 : 6); header.push_back(0); header.push_back(0); header.push_back(0);
    Chunk(png, "IHDR", header);
    if (indexed)
    {
        std::vector<uint8_t> plte, trns;
        for (uint32_t c : order)
        {
            const uint8_t *b = reinterpret_cast<const uint8_t *>(&c);
            plte.push_back(b[0]); plte.push_back(b[1]); plte.push_back(b[2]);
            trns.push_back(b[3]);
        }
        Chunk(png, "PLTE", plte);
        while (!trns.empty() && trns.back() == 255) trns.pop_back();
        if (!trns.empty()) Chunk(png, "tRNS", trns);
    }
    Chunk(png, "IDAT", packed);
    Chunk(png, "IEND", {});
    return true;
}

} // namespace exeicon
