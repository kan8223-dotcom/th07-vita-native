// Native Vita ARM mixer; SDL's Vita backend feeds SceAudioOut. The tested
// byte-oriented PCM decoder and logical sound mapping are platform independent.
// BGM (2026-10-13): thbgm.dat is read by a reader thread into a ring buffer; the audio
// callback only copies from the ring. Before, the callback read 1 KB from ux0 per block
// while holding the mutex the game thread takes every frame (ProcessQueues), and the game
// thread opened and measured thbgm.dat itself at every BGM start: a slow card read stalled
// the frame. Now no file operation runs on the game thread or in the callback.
#include "SoundPlayer.hpp"
#include "AudioPcm.hpp"
#include "FileSystem.hpp"
#include "Supervisor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <SDL2/SDL.h>
#include "Platform.hpp"
#include "CoreRuntime.hpp"

SoundBufferIdxVolume SOUND_BUFFER_IDX_VOL[38] = {
    {0,-2000,0},{0,-2500,0},{1,-1200,5},{1,-1500,5},{2,-1000,100},
    {3,-400,100},{4,-400,100},{5,-1500,50},{6,-1700,50},{7,-1900,50},
    {8,-1000,100},{9,-1000,100},{10,-1700,10},{11,-1200,10},{12,-900,100},
    {5,-1500,50},{13,-900,50},{14,-900,50},{15,-900,100},{16,-200,100},
    {17,-1400,0},{18,-1300,0},{5,-100,20},{6,-1800,20},{7,-1800,20},
    {19,-800,50},{20,-1000,50},{21,-1300,50},{22,-300,140},{23,-900,100},
    {24,-900,20},{25,-500,90},{26,-300,100},{27,-300,100},{24,-300,20},
    {19,0,50},{28,-300,100},{29,-300,100}};

const char *g_SFXList[30] = {
    "data/wav/se_plst00.wav","data/wav/se_enep00.wav","data/wav/se_pldead00.wav",
    "data/wav/se_power0.wav","data/wav/se_power1.wav","data/wav/se_tan00.wav",
    "data/wav/se_tan01.wav","data/wav/se_tan02.wav","data/wav/se_ok00.wav",
    "data/wav/se_cancel00.wav","data/wav/se_select00.wav","data/wav/se_gun00.wav",
    "data/wav/se_cat00.wav","data/wav/se_lazer00.wav","data/wav/se_lazer01.wav",
    "data/wav/se_enep01.wav","data/wav/se_nep00.wav","data/wav/se_damage00.wav",
    "data/wav/se_item00.wav","data/wav/se_kira00.wav","data/wav/se_kira01.wav",
    "data/wav/se_kira02.wav","data/wav/se_extend.wav","data/wav/se_timeout.wav",
    "data/wav/se_graze.wav","data/wav/se_powerup.wav","data/wav/se_border.wav",
    "data/wav/se_bonus.wav","data/wav/se_bonus2.wav","data/wav/se_pause.wav"};

SoundPlayer g_SoundPlayer;

// vita/VitaMidi.cpp: MIDI mode synthesized from the user's gm.dls subset.
extern "C" int th07_vita_midi_init(const char *blobPath);
extern "C" void th07_vita_midi_mix(int32_t *mix, int frames);

namespace {
using th07ps3audio::Read16;
using th07ps3audio::Read32;
constexpr unsigned kBlockFrames = 256;
// Keep SDL's threads and locks (sceKernel objects on the Vita) for audio. The
// rendering pipeline now also enables libstdc++ threads with pthread_cancel.
SDL_mutex *audioMutex;   // sound effects: voices, effects
SDL_AudioDeviceID audioDevice;
short callbackPcm[kBlockFrames * 2];
unsigned callbackOffset = sizeof(callbackPcm);
u32 fmtCount;
struct Voice { bool active = false; u32 cursor = 0; i32 gain = 0; } voices[38];
std::vector<int16_t> effects[30];

struct SdlLock {
    SDL_mutex *mutex;
    explicit SdlLock(SDL_mutex *m) : mutex(m) { if (mutex) SDL_LockMutex(mutex); }
    ~SdlLock() { if (mutex) SDL_UnlockMutex(mutex); }
};
struct Lock : SdlLock { Lock() : SdlLock(audioMutex) {} };

// ---- BGM: everything below is guarded by bgmMutex (never held across file I/O) ----
SDL_mutex *bgmMutex;
SDL_cond *bgmWake;
SDL_Thread *bgmReader;
bool bgmQuit;
// Made before the audio device and the reader exist (the game thread alone until then).
void CreateLocks()
{
    if (!audioMutex) audioMutex = SDL_CreateMutex();
    if (!bgmMutex) bgmMutex = SDL_CreateMutex();
    if (!bgmWake) bgmWake = SDL_CreateCond();
}
// Game thread -> reader: the track to stream. `bgmSerial` changes at every start or stop;
// ring data and reads of an older serial are dropped.
struct BgmTrack { bool play = false; char path[256] = {}; u32 base = 0, intro = 0, length = 0; };
BgmTrack bgmWanted;
bool bgmWantedTaken = true;
u32 bgmSerial;
// Reader -> callback: PCM bytes (44.1 kHz stereo s16) of the current track, loop included.
constexpr u32 kRingBytes = 512 * 1024, kReadBytes = 32 * 1024;   // ~3 s, ~0.19 s
std::vector<u8> bgmRing;
u32 bgmRingRead, bgmRingFill;
bool bgmActive, bgmEnded;   // a track is playing; its reads have stopped (error / short file)
bool bgmPaused, stageBlocked;
u32 fadeTotal, fadeRemaining;
u32 bgmStarvedBlocks;   // callback blocks that found the ring empty (slow card)

void CloseBgm()   // bgmMutex held
{
    bgmActive = bgmEnded = false;
    ++bgmSerial;
    bgmWanted.play = false;
    bgmWantedTaken = false;
    bgmRingRead = bgmRingFill = 0;
    bgmPaused = false;
    fadeTotal = fadeRemaining = 0;
    SDL_CondSignal(bgmWake);
}

int BgmReaderLoop(void *)
{
    touhou::vitacore::bind("bgm-reader",2);
    FILE *file = nullptr;
    BgmTrack track;
    u32 fileSerial = ~0u, position = 0;
    std::vector<u8> chunk(kReadBytes);
    SDL_LockMutex(bgmMutex);
    while (!bgmQuit)
    {
        if (!bgmWantedTaken)
        {
            bgmWantedTaken = true;
            track = bgmWanted;
            const u32 serial = bgmSerial;
            SDL_UnlockMutex(bgmMutex);
            if (file) std::fclose(file);
            file = nullptr;
            bool ok = !track.play;
            if (track.play && (file = std::fopen(track.path, "rb")) != nullptr)
            {
                std::setvbuf(file, nullptr, _IONBF, 0);   // whole kReadBytes reads, no second copy
                ok = std::fseek(file, long(track.base), SEEK_SET) == 0;
            }
            SDL_LockMutex(bgmMutex);
            if (serial == bgmSerial)
            {
                fileSerial = serial;
                position = 0;
                if (!ok) bgmEnded = true;
            }
            continue;
        }
        if (!(file && fileSerial == bgmSerial && bgmActive && !bgmEnded && kRingBytes - bgmRingFill >= kReadBytes))
        {
            SDL_CondWait(bgmWake, bgmMutex);
            continue;
        }
        bool seek = false;
        if (position >= track.length) { position = track.intro; seek = true; }
        const u32 want = std::min(kReadBytes, track.length - position), at = position, serial = bgmSerial;
        SDL_UnlockMutex(bgmMutex);
        bool ok = !seek || std::fseek(file, long(track.base + at), SEEK_SET) == 0;
        u32 got = ok ? u32(std::fread(chunk.data(), 1, want, file)) & ~3u : 0;
        SDL_LockMutex(bgmMutex);
        if (serial != bgmSerial) continue;   // stopped or restarted while reading
        const u32 tail = (bgmRingRead + bgmRingFill) % kRingBytes;
        const u32 first = std::min(got, kRingBytes - tail);
        std::memcpy(bgmRing.data() + tail, chunk.data(), first);
        std::memcpy(bgmRing.data(), chunk.data() + first, got - first);
        bgmRingFill += got;
        position = at + got;
        if (got < want) bgmEnded = true;   // as the inherited reader closed the BGM on a short read
    }
    SDL_UnlockMutex(bgmMutex);
    if (file) std::fclose(file);
    return 0;
}

void MixBgm(i32 *mix)
{
    SdlLock lock(bgmMutex);
    if (!bgmActive || bgmPaused || stageBlocked) return;
    unsigned produced = 0;
    for (; produced < kBlockFrames; ++produced)
    {
        if (bgmRingFill < 4)
        {
            if (bgmEnded) CloseBgm();
            else ++bgmStarvedBlocks;
            break;
        }
        const u8 *frame = bgmRing.data() + bgmRingRead;
        const i32 gain = fadeTotal ? i32(uint64_t(fadeRemaining) * 32768 / fadeTotal) : 32768;
        for (unsigned channel = 0; channel < 2; ++channel)
            mix[produced * 2 + channel] = i32(th07ps3audio::Signed16(frame + channel * 2)) * gain / 32768;
        bgmRingRead = (bgmRingRead + 4) % kRingBytes;
        bgmRingFill -= 4;
        if (fadeRemaining) --fadeRemaining;
        if (fadeTotal && !fadeRemaining) { CloseBgm(); ++produced; break; }
    }
    if (produced) SDL_CondSignal(bgmWake);
}

void MixBlock(short *output)
{
    i32 mix[kBlockFrames * 2] = {};
    MixBgm(mix);
    // MIDI BGM (silent unless MIDI mode plays); outside the effects lock.
    th07_vita_midi_mix(mix, kBlockFrames);
    Lock lock;
    for (unsigned sound = 0; sound < 38; ++sound)
    {
        auto &voice = voices[sound];
        if (!voice.active) continue;
        const auto &pcm = effects[SOUND_BUFFER_IDX_VOL[sound].bufferIdx];
        const u32 frames = pcm.size() / 2;
        const u32 count = std::min<u32>(kBlockFrames, frames - voice.cursor);
        for (u32 i = 0; i < count; ++i)
            for (unsigned channel = 0; channel < 2; ++channel)
                mix[i * 2 + channel] += i32(pcm[(voice.cursor + i) * 2 + channel]) * voice.gain / 32768;
        voice.cursor += count;
        if (voice.cursor >= frames) voice.active = false;
    }
    for (unsigned i = 0; i < kBlockFrames * 2; ++i)
        output[i] = static_cast<short>(std::max(-32768, std::min(32767, mix[i])));
}

void AudioCallback(void *, Uint8 *stream, int bytes)
{
    static bool affinityReady=false;
    if(!affinityReady){touhou::vitacore::bind("audio-mixer-output",2);affinityReady=true;}
    while (bytes > 0)
    {
        if (callbackOffset == sizeof(callbackPcm))
        {
            MixBlock(callbackPcm);
            callbackOffset = 0;
        }
        const int count = std::min<int>(bytes, sizeof(callbackPcm) - callbackOffset);
        std::memcpy(stream, reinterpret_cast<const Uint8 *>(callbackPcm) + callbackOffset, count);
        callbackOffset += count;
        stream += count;
        bytes -= count;
    }
}
} // namespace

SoundPlayer::SoundPlayer()
{
    std::memset(this, 0, sizeof(*this));
    for (auto &sound : soundQueue) sound = -1;
    for (auto &volume : unusedSoundVolRelated) volume = -1;
}

ZunResult SoundPlayer::InitializeSound()
{
    if (audioDevice) return ZUN_SUCCESS;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return ZUN_ERROR;
    stageBlocked = false;
    for (unsigned i = 0; i < 38; ++i)
        voices[i].gain = i32(std::pow(10.0, SOUND_BUFFER_IDX_VOL[i].volume / 2000.0) * 32768.0);
    SDL_AudioSpec wanted{};
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = kBlockFrames;
    wanted.callback = AudioCallback;
    // Before the audio thread starts: the instruments for the MIDI mode.
    if (th07_vita_midi_init("ux0:data/th07/midi-th07.tdsb"))
        th07_vita_log("MIDI instruments loaded (midi-th07.tdsb)");
    CreateLocks();
    {
        SdlLock lock(bgmMutex);
        bgmRing.assign(kRingBytes, 0);
        bgmQuit = false;
    }
    if (!bgmReader) bgmReader = SDL_CreateThread(BgmReaderLoop, "th07-bgm-reader", nullptr);
    audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, nullptr, 0);
    if (!audioDevice) return ZUN_ERROR;
    callbackOffset = sizeof(callbackPcm);
    SDL_PauseAudioDevice(audioDevice, 0);
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::Release()
{
    if (audioDevice)
    {
        SDL_CloseAudioDevice(audioDevice);
        audioDevice = 0;
    }
    {
        SdlLock lock(bgmMutex);
        CloseBgm();
        bgmQuit = true;
        SDL_CondSignal(bgmWake);
    }
    if (bgmReader) { SDL_WaitThread(bgmReader, nullptr); bgmReader = nullptr; }
    for (auto &voice : voices) voice.active = false;
    for (auto &pcm : effects) std::vector<int16_t>().swap(pcm);
    std::free(bgmFmtData);
    bgmFmtData = nullptr;
    fmtCount = 0;
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::LoadSound(i32 index, const char *path)
{
    if (index < 0 || index >= 30 || !path) return ZUN_ERROR;
    u8 *wav = FileSystem::OpenFile(path, 0);
    const u32 size = g_LastFileSize;
    std::vector<int16_t> pcm;
    const bool decoded = th07ps3audio::DecodeWave(wav, size, pcm);
    std::free(wav);
    if (!decoded) return ZUN_ERROR;
    Lock lock;
    for (unsigned i = 0; i < 38; ++i)
        if (SOUND_BUFFER_IDX_VOL[i].bufferIdx == index) voices[i].active = false;
    effects[index].swap(pcm);
    sfxFrameCount[index] = effects[index].size() / 2;
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::LoadFmt(const char *path)
{
    u8 *bytes = FileSystem::OpenFile(path, 0);
    const u32 size = g_LastFileSize;
    const u32 count = th07ps3audio::BgmFormatCount(bytes, size);
    if (!count)
    {
        std::free(bytes);
        return ZUN_ERROR;
    }
    auto *formats = static_cast<ThBgmFormat *>(std::calloc(count, sizeof(ThBgmFormat)));
    if (!formats) { std::free(bytes); return ZUN_ERROR; }
    for (u32 i = 0; i < count; ++i)
    {
        const u8 *p = bytes + i * 52;
        auto &f = formats[i];
        std::memcpy(f.name, p, 16);
        f.name[15] = 0;
        f.startOffset = Read32(p + 16);
        f.preloadAllocSize = Read32(p + 20);
        f.introLength = Read32(p + 24);
        f.totalLength = Read32(p + 28);
        f.format.wFormatTag = Read16(p + 32);
        f.format.nChannels = Read16(p + 34);
        f.format.nSamplesPerSec = Read32(p + 36);
        f.format.nAvgBytesPerSec = Read32(p + 40);
        f.format.nBlockAlign = Read16(p + 44);
        f.format.wBitsPerSample = Read16(p + 46);
        f.format.cbSize = Read16(p + 48);
    }
    std::free(bytes);
    Lock lock;
    std::free(bgmFmtData);
    bgmFmtData = formats;
    fmtCount = count;
    return ZUN_SUCCESS;
}

i32 SoundPlayer::GetFmtIndexByName(const char *path)
{
    if (!path || !bgmFmtData) return -1;
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    for (u32 i = 0; i < fmtCount; ++i)
        if (!std::strcmp(name, bgmFmtData[i].name)) return i;
    return -1;
}

ZunResult SoundPlayer::StartBGM(const char *path)
{
    if (!path || std::strlen(path) >= sizeof(bgmArchivePath)) return ZUN_ERROR;
    StopBGM();
    std::strcpy(bgmArchivePath, path);
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::ReopenBGM(const char *name)
{
    const i32 index = GetFmtIndexByName(name);
    if (index < 0 || g_Supervisor.cfg.musicMode == MUSIC_OFF) return ZUN_ERROR;
    const auto &fmt = bgmFmtData[index];
    if (fmt.startOffset < 0 || fmt.introLength < 0 || fmt.totalLength <= fmt.introLength ||
        (fmt.totalLength & 3) || (fmt.introLength & 3) ||
        fmt.format.wFormatTag != 1 || fmt.format.nChannels != 2 ||
        fmt.format.nSamplesPerSec != 44100 || fmt.format.wBitsPerSample != 16 ||
        fmt.format.nBlockAlign != 4 || bgmSeekOffset < 0)
        return ZUN_ERROR;
    const uint64_t base = uint64_t(bgmSeekOffset) + u32(fmt.startOffset);
    if (base + u32(fmt.totalLength) > 0x7fffffffu) return ZUN_ERROR;
    // The reader thread opens and seeks the archive; a missing or short file plays silence
    // (the inherited path measured the file here, on the game thread).
    SdlLock lock(bgmMutex);
    if (bgmStarvedBlocks) th07_vita_log("BGM previous track starved blocks=%u", bgmStarvedBlocks);
    bgmStarvedBlocks = 0;
    CloseBgm();
    bgmWanted.play = true;
    std::strncpy(bgmWanted.path, bgmArchivePath, sizeof(bgmWanted.path) - 1);
    bgmWanted.base = u32(base);
    bgmWanted.intro = fmt.introLength;
    bgmWanted.length = fmt.totalLength;
    bgmActive = true;
    curBgmIdx = index;
    return ZUN_SUCCESS;
}

ZunResult SoundPlayer::PreloadBGM(i32 index, const char *path)
{
    if (index < 0 || index >= 16 || !path || std::strlen(path) >= sizeof(bgmFileNames[index]))
        return ZUN_ERROR;
    // The PPU streams from HDD; keep only the requested track name, as on PSP.
    std::strcpy(bgmFileNames[index], path);
    return ZUN_SUCCESS;
}
ZunResult SoundPlayer::LoadBGM(i32 index)
{
    return index >= 0 && index < 16 ? ReopenBGM(bgmFileNames[index]) : ZUN_ERROR;
}
void SoundPlayer::StopBGM() { SdlLock lock(bgmMutex); CloseBgm(); }
void SoundPlayer::SetBgmStageLoadBlocked(bool blocked) { SdlLock lock(bgmMutex); stageBlocked = blocked; }
void SoundPlayer::FadeOut(f32 seconds)
{
    SdlLock lock(bgmMutex);
    if (!std::isfinite(seconds) || seconds <= 0) { CloseBgm(); return; }
    fadeTotal = fadeRemaining = u32(std::min(seconds, 3600.0f) * 44100.0f);
}

ZunResult SoundPlayer::InitSoundBuffers()
{
    for (auto &sound : soundQueue) sound = -1;
    for (i32 i = 0; i < 30; ++i)
        if (LoadSound(i, g_SFXList[i]) != ZUN_SUCCESS) return ZUN_ERROR;
    return ZUN_SUCCESS;
}
void SoundPlayer::PlaySoundByIdx(i32 index, u32)
{
    if (index < 0 || index >= 38) return;
    for (auto &sound : soundQueue)
    {
        if (sound == index) return;
        if (sound < 0) { sound = index; return; }
    }
}
void SoundPlayer::StopSoundByIdx(i32 index)
{
    if (index < 0 || index >= 38) return;
    Lock lock;
    voices[index].active = false;
    for (auto &sound : soundQueue) if (sound == index) sound = -1;
}
i32 SoundPlayer::ProcessQueues()
{
    const auto command = commandQueue[0];
    if (command.opcode)
    {
        switch (command.opcode)
        {
        case AUDIO_PRELOAD: PreloadBGM(command.arg1, command.string); break;
        case AUDIO_START:
            if (command.arg1 >= 0) LoadBGM(command.arg1); else ReopenBGM(command.string);
            break;
        case AUDIO_STOP: case AUDIO_SHUTDOWN: StopBGM(); break;
        case AUDIO_FADEOUT: FadeOut(f32(command.arg1)); break;
        case AUDIO_PAUSE: { SdlLock lock(bgmMutex); bgmPaused = true; break; }
        case AUDIO_UNPAUSE: { SdlLock lock(bgmMutex); bgmPaused = false; break; }
        default: break;
        }
        std::memmove(commandQueue, commandQueue + 1, sizeof(commandQueue) - sizeof(commandQueue[0]));
        commandQueue[31] = {};
    }
    Lock lock;
    for (auto &sound : soundQueue)
    {
        if (sound >= 0 && sound < 38 && g_Supervisor.cfg.playSounds)
        {
            voices[sound].cursor = 0;
            voices[sound].active = !effects[SOUND_BUFFER_IDX_VOL[sound].bufferIdx].empty();
        }
        sound = -1;
    }
    return commandQueue[0].opcode;
}
void SoundPlayer::PushCommand(AudioOpcode opcode, i32 argument, const char *text)
{
    for (auto &command : commandQueue)
    {
        if (command.opcode) continue;
        command = {};
        command.opcode = opcode;
        command.arg1 = argument;
        if (text) std::strncpy(command.string, text, sizeof(command.string) - 1);
        return;
    }
}
