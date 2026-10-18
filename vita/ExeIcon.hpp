// Read the icon out of the user's own Windows executable (RT_GROUP_ICON /
// RT_ICON), the same selection and decoding as tools/extract_exe_icons.py of
// touhou-modern-vita, and encode it as a 128x128 PNG (integer nearest-neighbour
// enlargement, exact palette + tRNS when the art has at most 256 RGBA values,
// RGBA8 otherwise).  Platform independent; no file access.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exeicon
{

struct Icon
{
    unsigned width = 0, height = 0, bpp = 0;
    unsigned groupId = 0, languageId = 0, resourceId = 0, ordinal = 0;
    std::vector<uint8_t> rgba; // width*height*4, top row first
};

// Largest original art, then highest depth (the Python tool's order). false + error on failure.
bool ExtractLargest(const uint8_t *exe, size_t size, Icon &icon, std::string &error);

// Enlarge by an integer factor to 128x128 (nearest neighbour). Fails if 128 % width != 0.
bool Enlarge128(const Icon &icon, std::vector<uint8_t> &rgba128, std::string &error);

// Encode 128x128 RGBA as PNG: indexed 8-bit + tRNS when <= 256 unique RGBA
// values (palette in first-seen order), else RGBA8.  indexed reports which.
bool EncodePng(const std::vector<uint8_t> &rgba, unsigned width, unsigned height, std::vector<uint8_t> &png,
               bool &indexed, std::string &error);

} // namespace exeicon
