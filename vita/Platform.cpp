#include "Platform.hpp"
#include "CoreRuntime.hpp"
#include "Controller.hpp"
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <deque>
#include <atomic>
#include <string>
#include <unistd.h>
#include <SDL2/SDL.h>

#ifndef TH07_VITA_VERSION
#define TH07_VITA_VERSION "dev"
#endif

extern "C" {
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;
FILE *__real_fopen(const char *, const char *);
}
namespace {
constexpr char kLogPath[] = "ux0:data/th07/vita-runtime.log";
std::atomic<bool> running{true};
unsigned presented = 0;
// Log lines are queued with their process time; a thread appends them (one open per batch).
// Start it before rendering can run on another core. Keep the existing SDL
// thread/lock implementation; the render worker separately links pthread_cancel
// so libstdc++'s thread and mutex implementation is active too.
bool logEnabled = false;
SDL_mutex *logMutex;
SDL_cond *logWake;
std::deque<std::string> logQueue;
SDL_Thread *logWriter;
bool logStopping = false;
void writeLogLines(const std::deque<std::string> &lines)
{
    if (lines.empty() || !logEnabled) return;
    FILE *file = __real_fopen(kLogPath, "ab");
    if (!file) return;
    for (const auto &line : lines) std::fputs(line.c_str(), file);
    std::fclose(file);
}
int logWriterLoop(void *)
{
    touhou::vitacore::bind("log-io",2);
    SDL_LockMutex(logMutex);
    for (;;)
    {
        while (!logStopping && logQueue.empty()) SDL_CondWait(logWake, logMutex);
        if (logQueue.empty() && logStopping) break;
        std::deque<std::string> batch;
        batch.swap(logQueue);
        SDL_UnlockMutex(logMutex);
        writeLogLines(batch);
        SDL_LockMutex(logMutex);
    }
    SDL_UnlockMutex(logMutex);
    return 0;
}
void startLogWriter()
{
    logMutex = SDL_CreateMutex();
    logWake = SDL_CreateCond();
    if (!logMutex || !logWake) return;
    SDL_Thread *thread = SDL_CreateThread(logWriterLoop, "th07-log", nullptr);
    if (!thread) return;
    SDL_LockMutex(logMutex);
    logWriter = thread;
    SDL_UnlockMutex(logMutex);
}
}
// Relative game paths resolve to ux0:data/th07.
extern "C" FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!path || !*path) { errno = EINVAL; return nullptr; }
    char resolved[1024];
    const int n = std::strchr(path, ':') || path[0] == '/' ?
        std::snprintf(resolved, sizeof(resolved), "%s", path) :
        std::snprintf(resolved, sizeof(resolved), "ux0:data/th07/%s", path);
    if (n < 0 || unsigned(n) >= sizeof(resolved)) { errno = ENAMETOOLONG; return nullptr; }
    for (char *p = resolved; *p; ++p) if (*p == '\\') *p = '/';
    return __real_fopen(resolved, mode);
}
void th07_vita_log(const char *format, ...)
{
    if (!logEnabled) return;
    char message[1024];
    const int prefix = std::snprintf(message, sizeof(message), "[TH07 Vita] t=%.3f ",
                                     sceKernelGetProcessTimeWide() / 1000000.0);
    va_list args; va_start(args, format);
    std::vsnprintf(message + prefix, sizeof(message) - prefix - 1, format, args);
    va_end(args);
    std::string line(message);
    line += '\n';
    if (logMutex)
    {
        SDL_LockMutex(logMutex);
        if (logWriter && !logStopping)
        {
            logQueue.push_back(std::move(line));
            SDL_CondSignal(logWake);
            SDL_UnlockMutex(logMutex);
            return;
        }
        SDL_UnlockMutex(logMutex);
    }
    writeLogLines(std::deque<std::string>{line});
}
void th07_vita_shutdown()
{
    if (!logMutex) return;
    SDL_LockMutex(logMutex);
    SDL_Thread *thread = logWriter;
    logStopping = true;
    SDL_CondSignal(logWake);
    SDL_UnlockMutex(logMutex);
    if (thread) SDL_WaitThread(thread, nullptr);
}
void th07_vita_stop(const char *reason)
{
    running = false;
    th07_vita_log("stop rendered=%u reason=%s", presented, reason);
}
void th07_vita_presented()
{
    ++presented;
}
void th07_vita_init()
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/th07", 0777);
    sceIoMkdir("ux0:data/th07/replay", 0777);
    chdir("ux0:data/th07");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    if (FILE *file = __real_fopen(kLogPath, "wb")) { std::fclose(file); logEnabled = true; }
    th07_vita_log("TH07 Vita %s (native ARM32 / GXM), data ux0:data/th07", TH07_VITA_VERSION);
    // The clocks of the measured runs; the game logic is unchanged by them.
    const int arm = scePowerSetArmClockFrequency(333), bus = scePowerSetBusClockFrequency(166),
              gpu = scePowerSetGpuClockFrequency(222), xbar = scePowerSetGpuXbarClockFrequency(166);
    th07_vita_log("CLOCKS set rc arm=%d bus=%d gpu=%d xbar=%d now arm=%d bus=%d gpu=%d xbar=%d", arm, bus, gpu, xbar,
                  scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(),
                  scePowerGetGpuXbarClockFrequency());
    touhou::vitacore::initialize(th07_vita_log);
    touhou::vitacore::bind("game-simulation",0);
    th07_vita_startup();
    SDL_SetMainReady();startLogWriter();
}
bool th07_vita_running() { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); return running; }
uint16_t th07_vita_buttons()
{
    // Same layout as the PSP port: Cross shot/OK, Circle bomb, Square/L/R focus,
    // Triangle skip, Start pause; d-pad or left stick past half travel moves.
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) return 0;
    uint16_t out = 0;
    if ((pad.buttons & SCE_CTRL_UP) || pad.ly < 64) out |= TH_BUTTON_UP;
    if ((pad.buttons & SCE_CTRL_DOWN) || pad.ly > 192) out |= TH_BUTTON_DOWN;
    if ((pad.buttons & SCE_CTRL_LEFT) || pad.lx < 64) out |= TH_BUTTON_LEFT;
    if ((pad.buttons & SCE_CTRL_RIGHT) || pad.lx > 192) out |= TH_BUTTON_RIGHT;
    if (pad.buttons & SCE_CTRL_CROSS) out |= TH_BUTTON_SHOOT | TH_BUTTON_ENTER;
    if (pad.buttons & SCE_CTRL_CIRCLE) out |= TH_BUTTON_BOMB;
    if (pad.buttons & (SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) out |= TH_BUTTON_FOCUS;
    if (pad.buttons & SCE_CTRL_TRIANGLE) out |= TH_BUTTON_SKIP;
    if (pad.buttons & SCE_CTRL_START) out |= TH_BUTTON_MENU;
    return out;
}
