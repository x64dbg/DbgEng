#include <cstdlib>
#include <cstdio>
#include <cstdint>

#include <DbgEng.h>
#include <delayimp.h>

// https://devblogs.microsoft.com/oldnewthing/20170126-00/?p=95265
static FARPROC WINAPI delayHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify == dliNotePreLoadLibrary)
    {
        if (_stricmp(pdli->szDll, "dbgeng.dll") == 0)
        {
            return (FARPROC)GetModuleHandleA("dbgeng.dll");
        }
        return (FARPROC)LoadLibraryA(pdli->szDll);
    }
    return 0;
}

const PfnDliHook __pfnDliNotifyHook2 = delayHook;

int main(int argc, char** argv)
{
    // TODO: load dbgeng.dll from the right path
    LoadLibraryA("dbgeng.dll");
    IDebugClient9* debugClient = nullptr;
    if (const auto result = DebugCreate(__uuidof(IDebugClient8), reinterpret_cast<void**>(&debugClient));
        result != S_OK)
    {
        printf("Failed to create IDebugClient9: %08x\n", (uint32_t)result);
        return EXIT_FAILURE;
    }
    printf("Created IDebugClient9\n");
    debugClient->Release();
    return EXIT_SUCCESS;
}
