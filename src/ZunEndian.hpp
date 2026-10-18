#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ZunEndian
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
constexpr bool kBigEndian = true;
#else
constexpr bool kBigEndian = false;
#endif

inline uint16_t Read16(const void *ptr)
{
    const auto *p = static_cast<const uint8_t *>(ptr);
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t Read32(const void *ptr)
{
    const auto *p = static_cast<const uint8_t *>(ptr);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void Write16(void *ptr, uint16_t value, bool bigEndian = false)
{
    auto *p = static_cast<uint8_t *>(ptr);
    p[bigEndian ? 1 : 0] = uint8_t(value);
    p[bigEndian ? 0 : 1] = uint8_t(value >> 8);
}
inline void Write32(void *ptr, uint32_t value, bool bigEndian = false)
{
    auto *p = static_cast<uint8_t *>(ptr);
    for (unsigned i = 0; i < 4; ++i)
        p[bigEndian ? 3 - i : i] = uint8_t(value >> (8 * i));
}
inline void ToNative16(void *ptr, bool bigEndian = kBigEndian)
{
    Write16(ptr, Read16(ptr), bigEndian);
}
inline void ToNative32(void *ptr, bool bigEndian = kBigEndian)
{
    Write32(ptr, Read32(ptr), bigEndian);
}
inline void Swap16(void *ptr)
{
    auto *p = static_cast<uint8_t *>(ptr);
    const uint8_t b = p[0]; p[0] = p[1]; p[1] = b;
}
inline void Swap32(void *ptr)
{
    auto *p = static_cast<uint8_t *>(ptr);
    const uint8_t a = p[0], b = p[1];
    p[0] = p[3]; p[1] = p[2]; p[2] = b; p[3] = a;
}
} // namespace ZunEndian
