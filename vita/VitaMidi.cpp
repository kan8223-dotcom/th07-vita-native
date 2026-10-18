// TH07 Vita: MIDI BGM synthesized from the user's own gm.dls (Microsoft GS
// Wavetable Synth instruments), the same design as the TH08 Vita build
// (~/th08_vita_r270_20261016/src/modern/vita/VitaMidi.cpp).  The game's
// MidiOutput is a stub in this tree (its timer and ProcessMsg are commented
// out), so nothing else sends MIDI events; on the Vita, and only when the
// instruments loaded, MidiOutput reads and hands the SMF files here and its
// Play/Stop/Fade calls are forwarded to touhou_dls_common's ZunMidiPlayer,
// which runs the same sequencer (ZunSequencer, written from TH06/TH07/TH08
// MidiOutput) on the audio thread and renders 22,050 Hz PCM that is repeated
// to 44,100 Hz.
//
// Instruments: midi-th07.tdsb in the data directory (ux0:data/th07/, TDSB v1
// subset of the user's gm.dls for the TH07 songs).  Without it the MIDI mode
// stays silent, exactly as before.
#include "tdls/zun_seq.hpp"

#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace
{
using namespace tdls;

Synth gSynth;
ZunSequencer gSequencer;
ZunCommandQueue gQueue;
ZunMidiPlayer gPlayer;
u8 *gBlob;
u8 *gArena;
volatile int gReady;
pthread_mutex_t gPushLock = PTHREAD_MUTEX_INITIALIZER;

// Songs handed to the player: copies, because the game frees its file buffers
// (LoadFile releases slot 31 right after parsing) while the audio thread may
// still read the playing song.  The player lets go of a song when the next
// PLAY is applied (1 ms later at most), so a ring of 8 never overwrites a song
// still in use.
enum { kSongSlots = 8 };
u8 *gSongs[kSongSlots];
u32 gSongCapacity[kSongSlots];
u32 gSongSize[kSongSlots];
int gNextSong;
int gParsedSong = -1;

s16 gRender[2 * 1024];

void Push(u32 type, u32 arg, const u8 *data, u32 size)
{
    if (!gReady) return;
    ZunCommand command = {type, arg, data, size};
    pthread_mutex_lock(&gPushLock);
    gQueue.push(command);
    pthread_mutex_unlock(&gPushLock);
}
} // namespace

extern "C" int th07_vita_midi_init(const char *blobPath)
{
    if (gReady) return 1;
    FILE *file = fopen(blobPath, "rb");
    if (file == NULL) return 0;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    gBlob = size > 0 ? static_cast<u8 *>(malloc(static_cast<size_t>(size))) : NULL;
    const bool read = gBlob != NULL && fread(gBlob, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size);
    fclose(file);
    TdsbView view;
    if (!read || tdsbOpen(gBlob, static_cast<u32>(size), view) != TDSB_OK) return 0;
    SynthConfig config;
    synthDefaultConfig(config);
    // 96 voices instead of MSGS's 32+8: MSGS steals the newest notes on
    // ch11-16 when busy, cutting e.g. the TH08 Eirin trumpet (20%) and the
    // TH06 U.N. Owen lead; the songs were written for the 64-voice SC-88Pro.
    // 96 removes every steal in all TH06/TH08 songs (田中さん 10-18 "途切れない").
    config.voices = 96;
    const u32 arenaBytes = Synth::memoryNeeded(view, config);
    gArena = static_cast<u8 *>(malloc(arenaBytes));
    if (gArena == NULL) return 0;
    Arena arena;
    arena.init(gArena, arenaBytes);
    if (!gSynth.init(view, config, arena)) return 0;
    if (!gPlayer.init(&gSynth, &gSequencer, &gQueue)) return 0;
    // 1 ms event timing (the TH06 PSP / TH08 Vita default).
    gReady = 1;
    return 1;
}

extern "C" int th07_vita_midi_ready(void) { return gReady; }

// MidiOutput::ParseFile succeeded on `size` bytes of SMF at `data`.
extern "C" void th07_vita_midi_parsed(const unsigned char *data, unsigned int size)
{
    if (!gReady || data == NULL || size == 0) return;
    const int slot = gNextSong;
    gNextSong = (gNextSong + 1) % kSongSlots;
    if (gSongCapacity[slot] < size)
    {
        free(gSongs[slot]);
        gSongs[slot] = static_cast<u8 *>(malloc(size));
        gSongCapacity[slot] = gSongs[slot] != NULL ? size : 0;
        if (gSongs[slot] == NULL) { gParsedSong = -1; return; }
    }
    memcpy(gSongs[slot], data, size);
    gSongSize[slot] = size;
    gParsedSong = slot;
}

extern "C" void th07_vita_midi_play(void)
{
    if (gParsedSong >= 0) Push(ZUN_CMD_PLAY, 0, gSongs[gParsedSong], gSongSize[gParsedSong]);
}

// Stop always precedes ParseFile in PlayLoaded / Play(path): forgetting the
// parsed song here keeps a failed parse from replaying the previous song.
extern "C" void th07_vita_midi_stop(void)
{
    gParsedSong = -1;
    Push(ZUN_CMD_STOP, 0, NULL, 0);
}

extern "C" void th07_vita_midi_fade(unsigned int ms) { Push(ZUN_CMD_FADE, ms, NULL, 0); }

// Audio thread: add `frames` stereo 44.1 kHz frames of MIDI into the 32-bit
// mix (the caller saturates once).  frames must be even.
extern "C" void th07_vita_midi_mix(int32_t *mix, int frames)
{
    if (!gReady || frames <= 0) return;
    while (frames > 0)
    {
        const int half = frames / 2 > 1024 ? 1024 : frames / 2;
        if (half == 0) return;
        gPlayer.render(gRender, static_cast<u32>(half));
        for (int i = 0; i < half; ++i)
            for (int repeat = 0; repeat < 2; ++repeat, mix += 2)
            {
                mix[0] += gRender[2 * i];
                mix[1] += gRender[2 * i + 1];
            }
        frames -= 2 * half;
    }
}
