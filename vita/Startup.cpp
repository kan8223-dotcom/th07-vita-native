// Boot steps before the game starts (vita/Platform.cpp th07_vita_init):
//  1. the original data must be in ux0:data/th07 - otherwise list what is
//     missing and wait for START, then exit;
//  2. once, the LiveArea icon from the user's own th07.exe / 東方妖々夢.exe;
//  3. with ux0:data/th07/gm.dls, the MIDI instrument set midi-th07.tdsb is
//     built (first time, or after gm.dls / th07.dat changed).
// Nothing here may stop the game except step 1.
#include "BootScreen.hpp"
#include "ExeIcon.hpp"
#include "MidiBuildGame.hpp"
#include "Platform.hpp"
#include "tdls/sha256.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <cstdio>
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef TH07_VITA_TITLEID
#error TH07_VITA_TITLEID must be defined by the build
#endif

namespace
{
constexpr char kDir[] = "ux0:data/th07/";

std::string DataPath(const char *name)
{
    return std::string(kDir) + name;
}

bool Stat(const std::string &path, SceIoStat &st)
{
    std::memset(&st, 0, sizeof(st));
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

bool ReadAll(const std::string &path, std::vector<uint8_t> &out)
{
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0)
    {
        th07_vita_log("STARTUP read %s open rc=0x%08x", path.c_str(), unsigned(fd));
        return false;
    }
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    bool ok = size > 0 && size < 64 * 1024 * 1024;
    if (ok)
    {
        out.resize(size_t(size));
        size_t done = 0;
        while (ok && done < out.size())
        {
            const int got = sceIoRead(fd, out.data() + done, out.size() - done > 1048576 ? 1048576 : out.size() - done);
            if (got <= 0)
            {
                th07_vita_log("STARTUP read %s rc=0x%08x at %u", path.c_str(), unsigned(got), unsigned(done));
                ok = false;
            }
            else
                done += size_t(got);
        }
    }
    sceIoClose(fd);
    return ok;
}

// Writes path.tmp, then replaces path.  Returns the first failing rc (0 = ok).
int WriteReplace(const std::string &path, const void *data, size_t size)
{
    const std::string temp = path + ".tmp";
    const SceUID fd = sceIoOpen(temp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return fd;
    size_t done = 0;
    int rc = 0;
    while (done < size)
    {
        const int wrote = sceIoWrite(fd, static_cast<const uint8_t *>(data) + done, size - done);
        if (wrote <= 0)
        {
            rc = wrote < 0 ? wrote : -1;
            break;
        }
        done += size_t(wrote);
    }
    const int closed = sceIoClose(fd);
    if (rc == 0 && closed < 0)
        rc = closed;
    if (rc != 0)
    {
        sceIoRemove(temp.c_str());
        return rc;
    }
    sceIoRemove(path.c_str());
    rc = sceIoRename(temp.c_str(), path.c_str());
    if (rc < 0)
        sceIoRemove(temp.c_str());
    return rc < 0 ? rc : 0;
}

// ---------------------------------------------------------------- 1. data
void CheckRequiredFiles()
{
    static const char *const required[] = {"th07.dat", "thbgm.dat"};
    std::vector<std::string> missing;
    for (const char *name : required)
    {
        SceIoStat st;
        if (!Stat(DataPath(name), st) || st.st_size <= 0)
            missing.push_back(name);
    }
    if (missing.empty())
    {
        th07_vita_log("STARTUP data files present");
        return;
    }
    for (const std::string &name : missing)
        th07_vita_log("STARTUP missing %s%s", kDir, name.c_str());
    std::vector<std::string> text = {"東方妖々夢のデータが見つかりません。", "Touhou 7 game data was not found.", "",
                                     "ux0:data/th07/ に置いてください / Copy into ux0:data/th07/:"};
    for (const std::string &name : missing)
        text.push_back("    " + name);
    text.push_back("");
    text.push_back("START で終了 / Press START to exit");
    std::vector<const char *> lines;
    for (const std::string &line : text)
        lines.push_back(line.c_str());
    if (bootscreen::Show(lines.data(), unsigned(lines.size())))
        bootscreen::WaitStart();
    th07_vita_log("MAIN EXIT result=missing-data");
    sceKernelExitProcess(0);
}

// ---------------------------------------------------------------- 2. icon
void InstallIcon()
{
    const std::string marker = DataPath("icon0-installed.txt");
    SceIoStat st;
    if (Stat(marker, st))
        return;
    // The executable keeps its own name in some installs.
    static const char *const names[] = {"th07.exe", "東方妖々夢.exe"};
    std::string exePath;
    for (const char *name : names)
        if (Stat(DataPath(name), st))
        {
            exePath = DataPath(name);
            break;
        }
    if (exePath.empty())
    {
        th07_vita_log("ICON th07.exe / 東方妖々夢.exe not in %s; keeping the bundled icon", kDir);
        return;
    }
    std::vector<uint8_t> exe, rgba, png;
    exeicon::Icon icon;
    std::string error;
    bool indexed = false;
    if (!ReadAll(exePath, exe) || !exeicon::ExtractLargest(exe.data(), exe.size(), icon, error) ||
        !exeicon::Enlarge128(icon, rgba, error) || !exeicon::EncodePng(rgba, 128, 128, png, indexed, error))
    {
        th07_vita_log("ICON extraction failed: %s; keeping the bundled icon", error.empty() ? "read" : error.c_str());
        const std::string note = "failed: " + error + "\n";
        WriteReplace(marker, note.data(), note.size());
        return;
    }
    uint8_t digest[32];
    char hex[65];
    tdls::sha256Buffer(png.data(), uint32_t(png.size()), digest);
    tdls::sha256Hex(digest, hex);
    th07_vita_log("ICON extracted from %s %ux%u %ubpp group=%u -> 128x128 %s PNG %u bytes sha256=%s", exePath.c_str(),
                  icon.width, icon.height, icon.bpp, icon.groupId, indexed ? "indexed" : "RGBA", unsigned(png.size()), hex);
    // The LiveArea reads the icon from ur0:appmeta (the copy in ux0:app is read-only).
    static const char target[] = "ur0:appmeta/" TH07_VITA_TITLEID "/icon0.png";
    const int rc = WriteReplace(target, png.data(), png.size());
    th07_vita_log("ICON write %s rc=0x%08x", target, unsigned(rc));
    char line[200];
    std::snprintf(line, sizeof(line), "%s rc=0x%08x\npng sha256=%s\n", target, unsigned(rc), hex);
    const int markerRc = WriteReplace(marker, line, std::strlen(line));
    th07_vita_log("ICON marker %s rc=0x%08x", marker.c_str(), unsigned(markerRc));
}

// ---------------------------------------------------------------- 3. MIDI
void MidiProgress(const char *stage, void *)
{
    th07_vita_log("MIDI BUILD %s", stage);
    const char *lines[] = {"MIDI の音色を準備しています（初回のみ）…", "Preparing MIDI instruments from gm.dls (first time only)...",
                           "", stage};
    bootscreen::Show(lines, 4);
}

void *BuildAlloc(uint32_t bytes, uint32_t align, void *)
{
    return memalign(align < 8 ? 8 : align, bytes ? bytes : 1);
}

void BuildRelease(void *p, void *)
{
    std::free(p);
}

std::string StampOf(const char *label, const SceIoStat &st)
{
    char text[128];
    const SceDateTime &t = st.st_mtime;
    std::snprintf(text, sizeof(text), "%s=%llu@%04u%02u%02u%02u%02u%02u.%06u", label, (unsigned long long)st.st_size,
                  t.year, t.month, t.day, t.hour, t.minute, t.second, t.microsecond);
    return text;
}

void PrepareMidi()
{
    const std::string dlsPath = DataPath("gm.dls");
    SceIoStat dlsStat, datStat;
    if (!Stat(dlsPath, dlsStat))
    {
        th07_vita_log("MIDI gm.dls not in %s; MIDI setting plays the WAV soundtrack unless midi-th07.tdsb exists", kDir);
        return;
    }
    if (!Stat(DataPath("th07.dat"), datStat))
        return;
    char head[96];
    std::snprintf(head, sizeof(head), "th07-midi-cache 1 builder=%u rule=C", unsigned(th07midi::kBuilderVersion));
    const std::string sources = std::string(head) + " " + StampOf("dls", dlsStat) + " " + StampOf("dat", datStat);
    const std::string stampPath = DataPath("midi-th07.stamp"), blobPath = DataPath("midi-th07.tdsb");
    {
        std::vector<uint8_t> old;
        SceIoStat blobStat;
        if (Stat(stampPath, blobStat) && ReadAll(stampPath, old))
        {
            const std::string line(old.begin(), old.end());
            if (line.compare(0, sources.size(), sources) == 0)
            {
                unsigned long long bytes = 0;
                const char *ok = std::strstr(line.c_str(), " ok bytes=");
                if (!ok)
                {
                    th07_vita_log("MIDI previous build from these files failed; not retrying (%s)", line.c_str());
                    return;
                }
                if (std::sscanf(ok, " ok bytes=%llu", &bytes) == 1 && Stat(blobPath, blobStat) &&
                    (unsigned long long)blobStat.st_size == bytes)
                {
                    th07_vita_log("MIDI midi-th07.tdsb is current (%llu bytes)", bytes);
                    return;
                }
            }
        }
    }
    th07_vita_log("MIDI building midi-th07.tdsb from gm.dls (%s)", sources.c_str());
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    MidiProgress("gm.dls", nullptr);
    std::vector<uint8_t> dls;
    th07midi::BuildResult result{};
    uint32_t songs = 0;
    bool ok = ReadAll(dlsPath, dls);
    if (ok)
    {
        uint8_t dlsSha[32];
        tdls::sha256Buffer(dls.data(), uint32_t(dls.size()), dlsSha);
        const th07midi::BuildAllocator allocator = {BuildAlloc, BuildRelease, nullptr};
        ok = th07midi::BuildGameBlob("th07.dat", dls.data(), uint32_t(dls.size()), dlsSha, allocator, result, &songs,
                                     MidiProgress, nullptr);
    }
    std::vector<uint8_t>().swap(dls);
    std::string stamp = sources;
    if (ok)
    {
        char hex[65];
        tdls::sha256Hex(result.blobSha256, hex);
        const int rc = WriteReplace(blobPath, result.blob, result.blobBytes);
        th07_vita_log("MIDI BUILD songs=%u tones=%u instruments=%u regions=%u waves=%u blob=%u bytes sha256=%s write rc=0x%08x %llu ms",
                      unsigned(songs), unsigned(result.tones), unsigned(result.instruments), unsigned(result.regions),
                      unsigned(result.waves), unsigned(result.blobBytes), hex, unsigned(rc),
                      (unsigned long long)((sceKernelGetProcessTimeWide() - t0) / 1000));
        char tail[128];
        std::snprintf(tail, sizeof(tail), " ok bytes=%u sha256=%s\n", unsigned(result.blobBytes), hex);
        stamp += tail;
        BuildRelease(result.blob, nullptr);
        if (rc != 0)
            return; // no stamp: try again next time
    }
    else
    {
        th07_vita_log("MIDI BUILD failed: %s; the MIDI setting plays the WAV soundtrack",
                      result.error ? result.error : "gm.dls could not be read");
        stamp += std::string(" failed ") + (result.error ? result.error : "read") + "\n";
        const char *lines[] = {"MIDI の準備に失敗しました。BGM は WAV で鳴らします。",
                               "MIDI preparation failed; the WAV soundtrack will be used.", "",
                               result.error ? result.error : "gm.dls could not be read"};
        bootscreen::Show(lines, 4);
        sceKernelDelayThread(3 * 1000 * 1000);
    }
    const int rc = WriteReplace(stampPath, stamp.data(), stamp.size());
    th07_vita_log("MIDI stamp rc=0x%08x", unsigned(rc));
}
} // namespace

void th07_vita_startup()
{
    CheckRequiredFiles();
    InstallIcon();
    PrepareMidi();
    bootscreen::Finish();
}
