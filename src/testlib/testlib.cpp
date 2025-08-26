#include <Windows.h>
#include "../testapp/debug.h"

extern "C" __declspec(dllexport) void myfunction()
{
    auto hFile = CreateFileA("example.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        auto size = GetFileSize(hFile, nullptr);
        dlogp("example.txt %u bytes", size);
        CloseHandle(hFile);
    }
    else
    {
        dlogp("failed to open example.txt!");
    }
}

// http://lallouslab.net/2017/05/30/using-cc-tls-callbacks-in-visual-studio-with-your-32-or-64bits-programs/
VOID WINAPI tls_callback(
    PVOID DllHandle,
    DWORD Reason,
    PVOID Reserved)
{
    dlogp("reason: %s", reason2str(Reason));
}

//-------------------------------------------------------------------------
// TLS 32/64 bits example by Elias Bachaalany <lallousz-x86@yahoo.com>
#ifdef _M_AMD64
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:p_tls_callback")
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_p_tls_callback")
#endif // _M_AMD64

#pragma const_seg(push)
#pragma const_seg(".CRT$XLAAA")
EXTERN_C const PIMAGE_TLS_CALLBACK p_tls_callback = tls_callback;
#pragma const_seg(pop)

BOOL WINAPI DllMain(
    HINSTANCE hinstDLL,
    DWORD fdwReason,
    LPVOID lpReserved)
{
    dlogp("reason: %s", reason2str(fdwReason));
    return TRUE;
}