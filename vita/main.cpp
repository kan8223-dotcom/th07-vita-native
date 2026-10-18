#include "Platform.hpp"
#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "GameWindow.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "ResultScreen.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include <SDL2/SDL.h>
#include <psp2/kernel/processmgr.h>
#include <cstdio>

static_assert(sizeof(void *) == 4 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Native Vita ARM32 LE");
#if !defined(__arm__) || defined(__mips__) || defined(TH07_PSP) || defined(TH07_PS3)
#error This executable must use native Vita ARM, never Allegrex/PSP/ME/PPU
#endif

bool AnmManager::TakeScreenshotIfRequested()
{
    if (screenshotTextureId < 0) return false;
    const bool result = TakeScreenshot(screenshotTextureId, screenshotSrcLeft, screenshotSrcTop,
        screenshotSrcWidth, screenshotSrcHeight, screenshotDstLeft, screenshotDstTop,
        screenshotDstWidth, screenshotDstHeight);
    screenshotTextureId = -1;
    return result;
}
static int RunGame(unsigned &tick, bool &configLoaded)
{
    if (g_Supervisor.LoadConfig("th07.cfg") != ZUN_SUCCESS) { th07_vita_log("config failed"); return 1; }
    configLoaded = true;
    g_Supervisor.cfg.frameskipConfig = 0;
    g_Supervisor.cfg.preloadBgm = 0;
    GameWindow::ChecksumExecutable();
    if (GameWindow::CreateGameWindow() || GameWindow::InitInterface() || GameWindow::InitRendering()) { th07_vita_log("graphics failed"); return 2; }
    th07_vita_log("graphics ready");
    g_GameWindow.frequency = SDL_GetPerformanceFrequency();
    if (g_SoundPlayer.InitializeSound() != ZUN_SUCCESS)
        th07_vita_log("audio initialization failed");
    Controller::ResetKeyboard();
    g_AnmManager = new AnmManager();
    if (g_Supervisor.RegisterChain() != ZUN_SUCCESS) { th07_vita_log("register chain failed"); return 3; }
    th07_vita_log("main loop ready");
    g_GameWindow.curFrame = -30;
    while (!g_GameWindow.isAppClosing && th07_vita_running())
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            if (event.type == SDL_QUIT) g_GameWindow.isAppClosing = true;
        const RenderResult result = g_GameWindow.Render();
        ++tick;
        if (result != RENDER_RESULT_KEEP_RUNNING) break;
        g_Supervisor.flags &= ~0x10u;
    }
    return 0;
}

int main()
{
    th07_vita_init();
    SDL_SetMainReady();
    unsigned tick = 0;
    bool configLoaded = false;
    // All initialization failures must join the same teardown path. Returning
    // directly would leave SDL's audio worker touching statically freed memory.
    const int result = RunGame(tick, configLoaded);
    th07_vita_log("MAIN STOP ticks=%u", tick);
    if (!result && g_GameManager.plst.base.magic) ResultScreen::RegisterChain(2);
    th07_vita_log("cleanup chain begin");
    g_Chain.Release();
    th07_vita_log("cleanup chain complete; drain audio commands");
    while (g_SoundPlayer.ProcessQueues()) {}
    th07_vita_log("cleanup audio begin");
    g_SoundPlayer.Release();
    th07_vita_log("cleanup audio complete; animation begin");
    delete g_AnmManager;
    g_AnmManager = nullptr;
    th07_vita_log("cleanup animation complete; config begin");
    // Save user settings independently of GPU teardown. Vita3K's Vulkan
    // context destruction can block; do not lose the selected screen mode.
    if (configLoaded) g_Supervisor.WriteConfig("th07.cfg");
    g_GameErrorContext.Flush();
    th07_vita_log("cleanup config complete; graphics begin");
    delete g_Supervisor.gfxDevice;
    g_Supervisor.gfxDevice = nullptr;
    th07_vita_log("cleanup graphics complete; SDL begin");
    SDL_Quit();
    th07_vita_log("MAIN EXIT ticks=%u result=%d", tick, result);
    th07_vita_shutdown();
    return sceKernelExitProcess(result);
}
