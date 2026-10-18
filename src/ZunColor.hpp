#pragma once

#include "inttypes.hpp"

union ZunColor {
    static u8 Multiply(u8 src, u8 factor)
    {
        u32 tmp = (u32)src * factor >> 7;
        if (tmp >= 256)
        {
            tmp = 255;
        }
        return tmp;
    }

    u32 color;
    struct ColorBytes
    {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        u8 a;
        u8 r;
        u8 g;
        u8 b;
#else
        u8 b;
        u8 g;
        u8 r;
        u8 a;
#endif
    } bytes;
    u8 raw[4];
};
