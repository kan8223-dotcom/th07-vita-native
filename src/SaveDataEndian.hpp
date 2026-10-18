#pragma once

#include "ZunEndian.hpp"
#include <algorithm>
#include <vector>

namespace SaveDataEndian
{
// These swaps are symmetric: host -> disk and disk -> host use the same
// layout. Raw character arrays and the one-byte game settings stay intact.
inline void Config(void *data, bool be = ZunEndian::kBigEndian)
{
    if (!be) return;
    auto *p = static_cast<uint8_t *>(data);
    for (size_t i = 0; i < 18; i += 2) ZunEndian::Swap16(p + i);
    ZunEndian::Swap32(p + 20);
    ZunEndian::Swap16(p + 24);
    ZunEndian::Swap16(p + 26);
    ZunEndian::Swap32(p + 52);
}

inline void ReplayHeader(void *data, bool be = ZunEndian::kBigEndian)
{
    if (!be) return;
    auto *p = static_cast<uint8_t *>(data);
    // The four magic bytes are compared/copied as a byte string.
    ZunEndian::Swap16(p + 4);
    ZunEndian::Swap32(p + 8);
    for (size_t i = 16; i < 0x54; i += 4) ZunEndian::Swap32(p + i);
}

inline bool ReplayPayload(void *data, size_t size, const uint32_t *inputOffsets,
                          const uint32_t *fpsOffsets, bool be = ZunEndian::kBigEndian)
{
    if (size < 0x94 || size > 32u * 1024u * 1024u) return false;
    std::vector<size_t> boundaries;
    for (size_t i = 0; i < 7; ++i)
    {
        if (inputOffsets[i]) boundaries.push_back(inputOffsets[i]);
        if (fpsOffsets[i]) boundaries.push_back(fpsOffsets[i]);
    }
    for (size_t offset : boundaries)
        if (offset < 0x54 + 0x94 || offset >= size + 0x54) return false;
    boundaries.push_back(size + 0x54);
    std::sort(boundaries.begin(), boundaries.end());
    auto *p = static_cast<uint8_t *>(data);
    for (size_t i = 0; i < 7; ++i)
    {
        const size_t start = inputOffsets[i];
        if (!start) continue;
        const size_t end = *std::upper_bound(boundaries.begin(), boundaries.end(), start);
        if (end - start < 44 || (end - start - 44) % 4) return false;
        for (size_t j = 0; j < i; ++j) if (inputOffsets[j] == start) return false;
        if (be)
        {
            auto *stage = p + start - 0x54;
            for (size_t j = 0; j < 32; j += 4) ZunEndian::Swap32(stage + j);
            ZunEndian::Swap16(stage + 32);
            ZunEndian::Swap32(stage + 40);
            for (size_t j = 44; j < end - start; j += 2) ZunEndian::Swap16(stage + j);
        }
    }
    // stageEndData is actually a byte stream of FPS samples, despite its
    // historical StageReplayData pointer type. Never swap those samples.
    if (be)
    {
        ZunEndian::Swap16(p + 22); ZunEndian::Swap32(p + 24); Config(p + 28, be);
        for (size_t i = 84; i < 140; i += 4) ZunEndian::Swap32(p + i);
        ZunEndian::Swap16(p + 144);
    }
    return true;
}

inline void ScoreHeader(void *data, bool be = ZunEndian::kBigEndian)
{
    if (!be) return;
    auto *p = static_cast<uint8_t *>(data);
    ZunEndian::Swap16(p + 2); ZunEndian::Swap16(p + 4);
    for (size_t i = 8; i < 28; i += 4) ZunEndian::Swap32(p + i);
}

inline bool ScoreChunks(void *data, size_t size, bool toDisk, bool be = ZunEndian::kBigEndian)
{
    auto *p = static_cast<uint8_t *>(data);
    for (size_t cursor = 0; cursor < size; )
    {
        if (size - cursor < 12) return false;
        auto *chunk = p + cursor;
        // Convert the header first when reading, last when writing, so the
        // native length/magic control traversal in both directions.
        if (be && !toDisk)
        {
            ZunEndian::Swap32(chunk); ZunEndian::Swap16(chunk + 4); ZunEndian::Swap16(chunk + 6);
        }
        uint32_t magic; uint16_t length;
        if (be)
        {
            magic = uint32_t(chunk[0]) << 24 | uint32_t(chunk[1]) << 16 |
                    uint32_t(chunk[2]) << 8 | chunk[3];
            length = uint16_t(chunk[4]) << 8 | chunk[5];
        }
        else { magic = ZunEndian::Read32(chunk); length = ZunEndian::Read16(chunk + 4); }
        if (length < 12 || length > size - cursor) return false;
        size_t required = 12;
        switch (magic)
        {
        case 0x4b544143: required = 0x78; break; // CATK
        case 0x52435348: required = 0x28; break; // HSCR
        case 0x44524c43: required = 0x1c; break; // CLRD
        case 0x52435350: required = 0x18; break; // PSCR
        case 0x54534c50: required = 0x160; break; // PLST
        case 0x4d4e534c: required = 0x18; break; // LSNM
        case 0x4d535256: required = 0x1c; break; // VRSM
        default: break;
        }
        if (length < required) return false;
        if (be)
        {
            switch (magic)
            {
            case 0x4b544143:
                for (size_t i = 12; i < 40; i += 4) ZunEndian::Swap32(chunk + i);
                ZunEndian::Swap16(chunk + 40);
                for (size_t i = 92; i < 120; i += 2) ZunEndian::Swap16(chunk + i);
                break;
            case 0x52435348: case 0x52435350:
                ZunEndian::Swap32(chunk + 12); ZunEndian::Swap32(chunk + 16); break;
            case 0x54534c50:
                for (size_t i = 12; i < 0x160; i += 4) ZunEndian::Swap32(chunk + i);
                break;
            case 0x4d535256:
                ZunEndian::Swap32(chunk + 20); ZunEndian::Swap32(chunk + 24); break;
            default: break;
            }
            if (toDisk)
            {
                ZunEndian::Swap32(chunk); ZunEndian::Swap16(chunk + 4); ZunEndian::Swap16(chunk + 6);
            }
        }
        cursor += length;
    }
    return true;
}
} // namespace SaveDataEndian
