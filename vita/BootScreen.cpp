#include "BootScreen.hpp"
#include "Platform.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

namespace bootscreen
{
namespace
{
bool g_Started;
bool g_Failed;
vita2d_font *g_Font;

bool Start()
{
    if (g_Started)
        return !g_Failed;
    g_Started = true;
    const int rc = vita2d_init_advanced(kTempPoolBytes);
    if (rc <= 0)
    {
        th07_vita_log("BOOTSCREEN vita2d_init_advanced rc=%d; text screen unavailable", rc);
        g_Failed = true;
        return false;
    }
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_set_vblank_wait(1);
    g_Font = vita2d_load_font_file("app0:NotoSansJP-Regular.ttf");
    if (!g_Font)
    {
        th07_vita_log("BOOTSCREEN font app0:NotoSansJP-Regular.ttf failed to load; text screen unavailable");
        g_Failed = true;
        return false;
    }
    th07_vita_log("BOOTSCREEN ready");
    return true;
}
} // namespace

bool Show(const char *const *lines, unsigned count)
{
    if (!Start())
        return false;
    // Two frames so both display buffers hold the text.
    for (int frame = 0; frame < 2; ++frame)
    {
        vita2d_start_drawing();
        vita2d_clear_screen();
        int y = 64;
        for (unsigned i = 0; i < count; ++i)
        {
            if (lines[i] && lines[i][0])
                vita2d_font_draw_text(g_Font, 48, y, RGBA8(240, 240, 240, 255), 24, lines[i]);
            y += 34;
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
    vita2d_wait_rendering_done();
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    return true;
}

void WaitStart()
{
    bool released = false;
    for (;;)
    {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const bool down = (pad.buttons & SCE_CTRL_START) != 0;
        if (!down)
            released = true;
        else if (released)
            return;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceKernelDelayThread(16 * 1000);
    }
}

void Finish()
{
    if (!g_Started || g_Failed)
        return;
    vita2d_wait_rendering_done();
    if (g_Font)
        vita2d_free_font(g_Font);
    g_Font = nullptr;
    th07_vita_log("BOOTSCREEN done; libvita2d stays initialized for the renderer");
}
} // namespace bootscreen
