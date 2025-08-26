#pragma once

#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>

inline void dprintf(const char* format, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, format);
    auto len = vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    for (; len != 0; len--)
    {
        auto& ch = msg[len - 1];
        if (ch == '\r' || ch == '\n')
            ch = '\0';
        else
            break;
    }
    OutputDebugStringA(msg);
    strcat_s(msg, "\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, strlen(msg), &written, nullptr);
}

inline void dputs(const char* text)
{
    dprintf("%s\n", text);
}

#define __FILENAME__ (strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#define dlog() dprintf("[debugme] [% 5u, % 5u] [%s] " __FUNCTION__ "\n", GetCurrentProcessId(), GetCurrentThreadId(), __FILENAME__)
#define dlogp(fmt, ...) dprintf("[debugme] [% 5u, % 5u] [%s] " __FUNCTION__ "(" fmt ")\n", GetCurrentProcessId(), GetCurrentThreadId(), __FILENAME__, __VA_ARGS__)

inline const char* reason2str(DWORD fdwReason)
{
    // Perform actions based on the reason for calling.
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        return "DLL_PROCESS_ATTACH";
    case DLL_THREAD_ATTACH:
        return "DLL_THREAD_ATTACH";
    case DLL_THREAD_DETACH:
        return "DLL_THREAD_DETACH";
    case DLL_PROCESS_DETACH:
        return "DLL_PROCESS_DETACH";
    }
    return "<unknown reason>";
}
