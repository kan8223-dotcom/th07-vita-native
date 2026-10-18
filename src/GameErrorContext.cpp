#include "GameErrorContext.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#if defined(TH07_VITA)
#include "Platform.hpp"
#endif

#if defined(TH07_PSP_SHIKIGAMI)
#include "shikigami_th07.h"
#endif

GameErrorContext g_GameErrorContext;

const char *GameErrorContext::Log(const char *fmt, ...)
{
    char tmp[8192];
    size_t tmpSize;
    va_list args;

    va_start(args, fmt);
    vsprintf(tmp, fmt, args);
#if defined(TH07_PS3) || defined(TH07_VITA)
    printf("[TH07] %s", tmp);
#endif
#if defined(TH07_VITA)
    th07_vita_log("%s", tmp);
#endif
    tmpSize = strlen(tmp);
    if (this->m_BufferEnd + tmpSize < this->m_Buffer + 0x1fff)
    {
        strcpy(this->m_BufferEnd, tmp);

        this->m_BufferEnd += tmpSize;
        *this->m_BufferEnd = '\0';
    }
    va_end(args);
    return fmt;
}

const char *GameErrorContext::Fatal(const char *fmt, ...)
{
    char tmp[512];
    size_t tmpSize;
    va_list args;

    va_start(args, fmt);
    vsprintf(tmp, fmt, args);
#if defined(TH07_PS3) || defined(TH07_VITA)
    printf("[TH07 fatal] %s", tmp);
#endif
#if defined(TH07_VITA)
    th07_vita_log("fatal: %s", tmp);
#endif
    tmpSize = strlen(tmp);
    if (this->m_BufferEnd + tmpSize < this->m_Buffer + 0x1fff)
    {
        strcpy(this->m_BufferEnd, tmp);
        this->m_BufferEnd += tmpSize;
        *this->m_BufferEnd = '\0';
    }
    va_end(args);
#if defined(TH07_PSP_SHIKIGAMI)
    th07_shikigami_record_fatal(tmp);
#endif
    this->m_ShowMessageBox = true;
    return fmt;
}
