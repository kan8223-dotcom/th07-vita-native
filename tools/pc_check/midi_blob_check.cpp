// PC check: build the TH07 gm.dls subset blob with the same code the Vita
// executable uses (vita/MidiBuildGame.cpp + vita/MidiBuild.cpp + the engine's
// Pbg4Archive) and print its SHA-256.
//   midi_blob_check <th07.dat> <gm.dls> [out.tdsb]
#include "MidiBuildGame.hpp"
#include "tdls/sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

static void *Alloc(tdls::u32 bytes, tdls::u32 align, void *)
{
    void *p = nullptr;
    if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, bytes ? bytes : 1) != 0)
        return nullptr;
    return p;
}
static void Release(void *p, void *) { std::free(p); }
static void Progress(const char *stage, void *) { std::fprintf(stderr, "progress: %s\n", stage); }

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <th07.dat> <gm.dls> [out.tdsb]\n", argv[0]);
        return 2;
    }
    FILE *f = std::fopen(argv[2], "rb");
    if (!f)
        return 2;
    std::vector<unsigned char> dls;
    unsigned char chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        dls.insert(dls.end(), chunk, chunk + got);
    std::fclose(f);
    tdls::u8 dlsSha[32];
    tdls::sha256Buffer(dls.data(), (tdls::u32)dls.size(), dlsSha);
    th07midi::BuildResult result;
    const th07midi::BuildAllocator allocator = {Alloc, Release, nullptr};
    tdls::u32 songs = 0;
    if (!th07midi::BuildGameBlob(argv[1], dls.data(), (tdls::u32)dls.size(), dlsSha, allocator, result, &songs,
                                 Progress, nullptr))
    {
        std::printf("FAIL %s\n", result.error ? result.error : "?");
        return 1;
    }
    char hex[65];
    tdls::sha256Hex(result.blobSha256, hex);
    std::printf("songs=%u tones=%u instruments=%u regions=%u waves=%u\n", songs, result.tones, result.instruments,
                result.regions, result.waves);
    std::printf("usage=%s\nlist=%s\nblob_bytes=%u\nblob_sha256=%s\n", result.usageSha, result.manifestSha,
                result.blobBytes, hex);
    if (argc > 3)
    {
        FILE *o = std::fopen(argv[3], "wb");
        if (!o || std::fwrite(result.blob, 1, result.blobBytes, o) != result.blobBytes)
            return 1;
        std::fclose(o);
    }
    std::free(result.blob);
    return 0;
}
