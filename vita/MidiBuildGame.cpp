#include "MidiBuildGame.hpp"

#include "pbg4/Pbg4Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace th07midi
{

namespace
{
constexpr u32 kMaxSongs = 32;
constexpr u32 kMaxFileBytes = 1024u * 1024u;

u8 *ReadEntry(Pbg4Archive &archive, const char *name, u32 size)
{
    if (size == 0 || size > kMaxFileBytes)
        return nullptr;
    u8 *data = static_cast<u8 *>(std::malloc(size));
    if (data && !archive.ReadDecompressEntry(name, data))
    {
        std::free(data);
        data = nullptr;
    }
    return data;
}
} // namespace

bool BuildGameBlob(const char *datPath, const u8 *dls, u32 dlsBytes, const u8 dlsSha256[32],
                   const BuildAllocator &allocator, BuildResult &out, u32 *songCount, BuildProgress progress,
                   void *progressUser)
{
    std::memset(&out, 0, sizeof(out));
    if (songCount)
        *songCount = 0;
    if (progress)
        progress("MIDI READ", progressUser);
    Pbg4Archive *archive = new (std::nothrow) Pbg4Archive();
    if (!archive || !archive->Load(datPath))
    {
        delete archive;
        out.error = "th07.dat is not readable";
        return false;
    }
    BuildFile files[kMaxSongs] = {};
    char names[kMaxSongs][24] = {};
    u32 count = 0;
    bool readFailed = false;
    // Entries are enumerated rather than named one by one: th07_13b.mid is a song too.
    for (i32 i = 0; i < archive->numOfEntries && count < kMaxSongs; ++i)
    {
        const char *name = archive->entries[i].filename;
        if (!name || std::strncmp(name, "th07_", 5) != 0)
            continue;
        const char *extension = std::strrchr(name, '.');
        if (!extension || std::strcmp(extension, ".mid") != 0 || std::strlen(name) >= sizeof(names[0]))
            continue;
        const u32 size = archive->entries[i].decompressedSize;
        std::snprintf(names[count], sizeof(names[count]), "%s", name);
        u8 *data = ReadEntry(*archive, names[count], size);
        if (!data)
        {
            readFailed = true;
            break;
        }
        files[count].name = names[count];
        files[count].data = data;
        files[count].size = size;
        ++count;
    }
    BuildFile init = {"init.mid", nullptr, 0};
    if (!readFailed)
    {
        init.size = archive->GetEntrySize("init.mid");
        init.data = ReadEntry(*archive, "init.mid", init.size);
    }
    delete archive;
    // The PC tool sorts by base name (strcmp order: th07_13.mid < th07_13b.mid).
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j && std::strcmp(files[j - 1].name, files[j].name) > 0; --j)
        {
            const BuildFile previous = files[j - 1];
            files[j - 1] = files[j];
            files[j] = previous;
        }
    if (songCount)
        *songCount = count;
    bool ok = false;
    if (readFailed)
        out.error = "th07.dat could not be read";
    else if (count != kGameSongCount)
        out.error = "th07.dat does not hold the 20 songs of TH07 1.00b";
    else if (!init.data)
        out.error = "init.mid is not in th07.dat";
    else
        ok = BuildBlob(dls, dlsBytes, dlsSha256, files, count, "th07", kGameRule, allocator, out, progress,
                       progressUser, &init);
    for (u32 i = 0; i < count; ++i)
        std::free(const_cast<u8 *>(files[i].data));
    std::free(const_cast<u8 *>(init.data));
    return ok;
}

} // namespace th07midi
