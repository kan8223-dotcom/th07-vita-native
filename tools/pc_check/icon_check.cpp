// PC check: run the Vita executable's icon extractor (vita/ExeIcon.cpp) on a
// Windows executable and print the hashes compared with extract_exe_icons.py.
//   icon_check <original.exe> <out.png>
#include "ExeIcon.hpp"
#include "tdls/sha256.hpp"

#include <cstdio>
#include <vector>

static std::string Hex(const std::vector<uint8_t> &v)
{
    tdls::u8 d[32];
    char hex[65];
    tdls::sha256Buffer(v.data(), (tdls::u32)v.size(), d);
    tdls::sha256Hex(d, hex);
    return hex;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::vector<uint8_t> exe;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) exe.insert(exe.end(), chunk, chunk + got);
    std::fclose(f);
    exeicon::Icon icon;
    std::string error;
    std::vector<uint8_t> big, png;
    bool indexed = false;
    if (!exeicon::ExtractLargest(exe.data(), exe.size(), icon, error) || !exeicon::Enlarge128(icon, big, error) ||
        !exeicon::EncodePng(big, 128, 128, png, indexed, error))
    {
        std::printf("FAIL %s\n", error.c_str());
        return 1;
    }
    std::printf("selected %ux%u %ubpp group=%u language=%u resource=%u ordinal=%u\n", icon.width, icon.height, icon.bpp,
                icon.groupId, icon.languageId, icon.resourceId, icon.ordinal);
    std::printf("rgba32_sha256=%s\nrgba128_sha256=%s\npng=%s bytes=%zu sha256=%s\n", Hex(icon.rgba).c_str(),
                Hex(big).c_str(), indexed ? "indexed" : "rgba", png.size(), Hex(png).c_str());
    FILE *o = std::fopen(argv[2], "wb");
    if (!o || std::fwrite(png.data(), 1, png.size(), o) != png.size()) return 1;
    std::fclose(o);
    return 0;
}
