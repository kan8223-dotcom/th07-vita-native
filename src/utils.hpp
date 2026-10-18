#pragma once

#include "ZunMath.hpp"
#include "inttypes.hpp"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

union AnyArg {
    i32 i;
    u32 u;
    f32 f;
    i16 s[2];
    u16 us[2];
    i8 c[4];
    u8 b[4];

    // Script arguments are host-order words after loading. Packed byte/half
    // operands retain the original x86 numbering on PowerPC as well.
    u8 Byte(unsigned index) const { return static_cast<u8>(u >> (index * 8)); }
    i8 SignedByte(unsigned index) const { return static_cast<i8>(Byte(index)); }
    u16 Half(unsigned index) const { return static_cast<u16>(u >> (index * 16)); }
    i16 SignedHalf(unsigned index) const { return static_cast<i16>(Half(index)); }
};
static_assert(sizeof(AnyArg) == 4);

namespace utils
{
f32 AddNormalizeAngle(f32 a, f32 b);
void Rotate(ZunVec3 *out, ZunVec3 *point, f32 angle);

inline f32 NormalizeAngle(f32 a)
{
    return AddNormalizeAngle(a, 0.0f);
}
} // namespace utils
