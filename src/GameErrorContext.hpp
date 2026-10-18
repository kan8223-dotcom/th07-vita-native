#pragma once

#include <SDL2/SDL.h>
#include <cstddef>

#include "FileSystem.hpp"

struct GameErrorContext
{
    char m_Buffer[8192];
    char *m_BufferEnd;
    i8 m_ShowMessageBox;

    GameErrorContext()
    {
        m_BufferEnd = m_Buffer;
        m_Buffer[0] = '\0';
        m_ShowMessageBox = false;
        Log("東方動作記録 --------------------------------------------- \n");
    }

    const char *Fatal(const char *fmt, ...);
    const char *Log(const char *fmt, ...);

    void Flush()
    {
        if (this->m_BufferEnd != this->m_Buffer)
        {
            this->Log("---------------------------------------------------------- \n");
            // The Vita port has no SDL window/message-box subsystem; every line
            // already went to its runtime log (vita-runtime.log), so no log.txt.
#if !defined(TH07_VITA)
            if (this->m_ShowMessageBox)
            {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "log", this->m_Buffer, NULL);
            }
            FileSystem::WriteDataToFile("./log.txt", this->m_Buffer, strlen(this->m_Buffer));
#endif
        }
    }
};
extern GameErrorContext g_GameErrorContext;
