#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace th07ps3audio {
inline uint16_t Read16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t Read32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline int16_t Signed16(const uint8_t *p)
{
    const uint16_t bits = Read16(p);
    return static_cast<int16_t>(bits < 0x8000 ? int32_t(bits) : int32_t(bits) - 65536);
}

inline uint32_t BgmFormatCount(const uint8_t *bytes, size_t size)
{
    if (!bytes || size < 52 || size / 52 > 256) return 0;
    // Retail thbgm.fmt ends with a zero filename sentinel (17 bytes), not
    // another complete 52-byte ThBgmFormat record.
    const uint32_t count = static_cast<uint32_t>(size / 52);
    for (size_t i = size_t(count) * 52; i < size; ++i)
        if (bytes[i]) return 0;
    return count;
}

// Decode the retail PCM WAV effects into native-endian stereo 44.1 kHz.
// RIFF integers and PCM samples are explicitly little-endian, including on PPU.
inline bool DecodeWave(const uint8_t *wav, size_t size, std::vector<int16_t> &out)
{
    out.clear();
    if (!wav || size < 12 || std::memcmp(wav, "RIFF", 4) ||
        std::memcmp(wav + 8, "WAVE", 4))
        return false;
    const uint64_t declared = uint64_t(Read32(wav + 4)) + 8;
    if (declared > size || declared < 12)
        return false;
    size = static_cast<size_t>(declared);
    const uint8_t *format = nullptr, *pcm = nullptr;
    uint32_t formatSize = 0, pcmBytes = 0;
    for (size_t offset = 12; size - offset >= 8;)
    {
        const uint32_t bytes = Read32(wav + offset + 4);
        const size_t payload = offset + 8;
        if (bytes > size - payload)
            return false;
        if (!std::memcmp(wav + offset, "fmt ", 4))
        {
            format = wav + payload;
            formatSize = bytes;
        }
        else if (!std::memcmp(wav + offset, "data", 4))
        {
            pcm = wav + payload;
            pcmBytes = bytes;
        }
        const uint64_t next = uint64_t(payload) + bytes + (bytes & 1u);
        if (next > size)
            break;
        offset = static_cast<size_t>(next);
    }
    if (!format || formatSize < 16 || !pcm || Read16(format) != 1)
        return false;
    const uint32_t channels = Read16(format + 2), rate = Read32(format + 4);
    const uint32_t align = Read16(format + 12), bits = Read16(format + 14);
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16) ||
        rate < 8000 || rate > 192000 || align != channels * (bits / 8) ||
        !pcmBytes || pcmBytes % align)
        return false;
    const uint32_t inputFrames = pcmBytes / align;
    const uint64_t outputFrames = (uint64_t(inputFrames) * 44100 + rate - 1) / rate;
    if (outputFrames > 44100u * 60u)
        return false;
    out.resize(static_cast<size_t>(outputFrames) * 2);
    auto sample = [&](uint32_t frame, uint32_t channel) -> int32_t {
        const uint8_t *p = pcm + size_t(frame) * align + (channels == 1 ? 0 : channel * (bits / 8));
        return bits == 8 ? (int32_t(*p) - 128) * 256 : Signed16(p);
    };
    for (uint64_t i = 0; i < outputFrames; ++i)
    {
        const uint64_t position = i * rate;
        const uint32_t frame = static_cast<uint32_t>(position / 44100);
        const uint32_t next = std::min(frame + 1, inputFrames - 1);
        const int32_t fraction = static_cast<int32_t>(position % 44100);
        for (uint32_t channel = 0; channel < 2; ++channel)
        {
            const int32_t a = sample(frame, channel), b = sample(next, channel);
            out[size_t(i) * 2 + channel] = static_cast<int16_t>(
                a + int64_t(b - a) * fraction / 44100);
        }
    }
    return true;
}
} // namespace th07ps3audio
