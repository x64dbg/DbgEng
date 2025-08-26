#include <cstdio>
#include <cstdlib>
#include <Windows.h>
#include "debug.h"

// imported from testlib.dll
extern "C" __declspec(dllimport) void myfunction();

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

static DWORD WINAPI MyThread(PVOID parameter)
{
    dlogp("parameter: %p", parameter);
    Sleep(200);
    return 0;
}

int main()
{
    dlogp("entry point");
    myfunction();
    dlogp("== creating thread ==");
    auto hThread = CreateThread(nullptr, 0, MyThread, (void*)0x1337, 0, nullptr);
    WaitForSingleObject(hThread, INFINITE);
    dlogp("== thread finished! ==");
    puts("\nIt's working!\n");
    return EXIT_SUCCESS;
}
