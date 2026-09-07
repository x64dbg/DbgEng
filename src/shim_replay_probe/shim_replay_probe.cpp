#include "../TitanEngine/TitanEngine.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <string>

namespace
{

HANDLE gReadyEvent = nullptr;
HANDLE gContinueEvent = nullptr;
HANDLE gProcess = nullptr;
HANDLE gThread = nullptr;
ULONG_PTR gImageBase = 0;
TitanSessionKind gExpectedKind = UE_SESSION_MINIDUMP;
std::atomic<unsigned> gThreadCount = 0;
std::atomic<unsigned> gModuleCount = 0;

void onCreateProcess(const void* argument)
{
    const auto* info = static_cast<const CREATE_PROCESS_DEBUG_INFO*>(argument);
    gProcess = info->hProcess;
    gThread = info->hThread;
    gImageBase = reinterpret_cast<ULONG_PTR>(info->lpBaseOfImage);
}

void onCreateThread(const void*)
{
    ++gThreadCount;
}

void onLoadModule(const void*)
{
    ++gModuleCount;
}

void onSystemBreakpoint(const void*)
{
    SetEvent(gReadyEvent);
    WaitForSingleObject(gContinueEvent, 30000);
}

struct Api
{
    decltype(&EngineCheckStructAlignment) EngineCheckStructAlignment = nullptr;
    decltype(&InitReplayW) InitReplayW = nullptr;
    decltype(&GetSessionInfo) GetSessionInfo = nullptr;
    decltype(&ReplayGetPosition) ReplayGetPosition = nullptr;
    decltype(&ReplayGetExtent) ReplayGetExtent = nullptr;
    decltype(&ReplaySetPosition) ReplaySetPosition = nullptr;
    decltype(&ReplayStepBack) ReplayStepBack = nullptr;
    decltype(&SetCustomHandler) SetCustomHandler = nullptr;
    decltype(&TitanCloseHandle) TitanCloseHandle = nullptr;
    decltype(&DebugLoop) DebugLoop = nullptr;
    decltype(&StopDebug) StopDebug = nullptr;
    decltype(&GetFullContextDataEx) GetFullContextDataEx = nullptr;
    decltype(&MemoryReadSafe) MemoryReadSafe = nullptr;
    decltype(&MemoryQuerySafe) MemoryQuerySafe = nullptr;
    decltype(&MemoryWriteSafe) MemoryWriteSafe = nullptr;
    decltype(&GetPEBLocation) GetPEBLocation = nullptr;
    decltype(&GetTEBLocation) GetTEBLocation = nullptr;
};

Api gApi;
std::atomic<DWORD> gWorkerError = ERROR_SUCCESS;

DWORD WINAPI worker(void* artifact)
{
    gApi.SetCustomHandler(UE_CH_CREATEPROCESS, onCreateProcess);
    gApi.SetCustomHandler(UE_CH_CREATETHREAD, onCreateThread);
    gApi.SetCustomHandler(UE_CH_LOADDLL, onLoadModule);
    gApi.SetCustomHandler(UE_CH_SYSTEMBREAKPOINT, onSystemBreakpoint);

    auto* info = gApi.InitReplayW(static_cast<const wchar_t*>(artifact), gExpectedKind);
    if(!info)
    {
        gWorkerError = GetLastError();
        SetEvent(gReadyEvent);
        return 1;
    }
    gApi.TitanCloseHandle(info->hProcess);
    gApi.TitanCloseHandle(info->hThread);
    gApi.DebugLoop();
    return 0;
}

template<typename T>
bool resolve(HMODULE module, const char* name, T& output)
{
    output = reinterpret_cast<T>(GetProcAddress(module, name));
    if(!output)
        std::printf("missing export: %s\n", name);
    return output != nullptr;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if(argc != 2)
    {
        std::fwprintf(stderr, L"usage: shim_replay_probe <minidump-or-trace>\n");
        return 2;
    }
    const std::wstring artifact = argv[1];
    if(artifact.size() >= 4 && _wcsicmp(artifact.c_str() + artifact.size() - 4, L".run") == 0)
        gExpectedKind = UE_SESSION_TTD;

    const auto module = LoadLibraryW(L"TitanEngine.dll");
    if(!module)
    {
        std::printf("LoadLibraryW(TitanEngine.dll) failed: %lu\n", GetLastError());
        return 1;
    }

#define RESOLVE(name) if(!resolve(module, #name, gApi.name)) return 1
    RESOLVE(EngineCheckStructAlignment);
    RESOLVE(InitReplayW);
    RESOLVE(GetSessionInfo);
    RESOLVE(ReplayGetPosition);
    RESOLVE(ReplayGetExtent);
    RESOLVE(ReplaySetPosition);
    RESOLVE(ReplayStepBack);
    RESOLVE(SetCustomHandler);
    RESOLVE(TitanCloseHandle);
    RESOLVE(DebugLoop);
    RESOLVE(StopDebug);
    RESOLVE(GetFullContextDataEx);
    RESOLVE(MemoryReadSafe);
    RESOLVE(MemoryQuerySafe);
    RESOLVE(MemoryWriteSafe);
    RESOLVE(GetPEBLocation);
    RESOLVE(GetTEBLocation);
#undef RESOLVE

    if(!gApi.EngineCheckStructAlignment(UE_STRUCT_TITAN_ENGINE_CONTEXT, sizeof(TITAN_ENGINE_CONTEXT_t)))
    {
        std::printf("EngineCheckStructAlignment failed: %lu\n", GetLastError());
        return 1;
    }

    TITAN_SESSION_INFO before = {};
    if(!gApi.GetSessionInfo(&before) || before.kind != UE_SESSION_NONE)
    {
        std::printf("invalid initial session state\n");
        return 1;
    }

    gReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gContinueEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto thread = CreateThread(nullptr, 0, worker, argv[1], 0, nullptr);
    if(!thread || WaitForSingleObject(gReadyEvent, 30000) != WAIT_OBJECT_0)
    {
        std::printf("replay startup timed out\n");
        return 1;
    }
    if(gWorkerError != ERROR_SUCCESS)
    {
        std::printf("InitReplayW failed: %lu\n", gWorkerError.load());
        WaitForSingleObject(thread, 30000);
        return 1;
    }

    TITAN_SESSION_INFO session = {};
    if(!gApi.GetSessionInfo(&session))
    {
        std::printf("GetSessionInfo failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("session kind=%u caps=0x%llx machine=0x%x pid=%lu tid=%lu\n",
                static_cast<unsigned>(session.kind), static_cast<unsigned long long>(session.capabilities),
                session.machineType, session.processId, session.threadId);
    std::printf("events image=%p threads=%u modules=%u\n", reinterpret_cast<void*>(gImageBase),
                gThreadCount.load(), gModuleCount.load());

    if(gExpectedKind == UE_SESSION_TTD)
    {
        TITAN_REPLAY_POSITION first = {}, last = {}, current = {};
        if(!gApi.ReplayGetExtent(&first, &last) || !gApi.ReplayGetPosition(&current))
        {
            std::printf("TTD position query failed: %lu\n", GetLastError());
            return 1;
        }
        std::printf("timeline first=%llx:%llx current=%llx:%llx last=%llx:%llx\n",
                    first.sequence, first.steps, current.sequence, current.steps, last.sequence, last.steps);
        if(!gApi.ReplaySetPosition(&last) || !gApi.ReplayGetPosition(&current) ||
           current.sequence != last.sequence || current.steps != last.steps ||
           !gApi.ReplayStepBack(nullptr) || !gApi.ReplaySetPosition(&first))
        {
            std::printf("TTD exact seek round trip failed: %lu\n", GetLastError());
            return 1;
        }
    }

    TITAN_ENGINE_CONTEXT_t context = {};
    if(!gApi.GetFullContextDataEx(gThread, &context))
    {
        std::printf("GetFullContextDataEx failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("context cip=%p csp=%p peb=%p teb=%p\n", reinterpret_cast<void*>(context.cip),
                reinterpret_cast<void*>(context.csp), reinterpret_cast<void*>(gApi.GetPEBLocation(gProcess)),
                reinterpret_cast<void*>(gApi.GetTEBLocation(gThread)));

    std::array<unsigned char, 16> bytes = {};
    SIZE_T bytesRead = 0;
    if(!gApi.MemoryReadSafe(gProcess, reinterpret_cast<void*>(context.cip), bytes.data(), bytes.size(), &bytesRead))
    {
        std::printf("MemoryReadSafe failed: %lu (%llu bytes)\n", GetLastError(),
                    static_cast<unsigned long long>(bytesRead));
        return 1;
    }
    std::printf("memory bytes=");
    for(const auto byte : bytes)
        std::printf("%02x", byte);
    std::putchar('\n');

    MEMORY_BASIC_INFORMATION memory = {};
    if(gApi.MemoryQuerySafe(gProcess, reinterpret_cast<void*>(context.cip), &memory, sizeof(memory)) != sizeof(memory))
    {
        std::printf("MemoryQuerySafe failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("region base=%p size=0x%llx protect=0x%lx\n", memory.BaseAddress,
                static_cast<unsigned long long>(memory.RegionSize), static_cast<unsigned long>(memory.Protect));

    SIZE_T written = 0;
    const unsigned char replacement = 0x90;
    if(gApi.MemoryWriteSafe(gProcess, reinterpret_cast<void*>(context.cip), &replacement, 1, &written) ||
       GetLastError() != ERROR_NOT_SUPPORTED)
    {
        std::printf("immutable MemoryWriteSafe contract failed: result bytes=%llu error=%lu\n",
                    static_cast<unsigned long long>(written), GetLastError());
        return 1;
    }

    SetEvent(gContinueEvent);
    if(!gApi.StopDebug())
    {
        std::printf("StopDebug failed: %lu\n", GetLastError());
        return 1;
    }
    const auto wait = WaitForSingleObject(thread, 30000);
    DWORD workerExit = 1;
    GetExitCodeThread(thread, &workerExit);
    CloseHandle(thread);
    CloseHandle(gContinueEvent);
    CloseHandle(gReadyEvent);
    if(wait != WAIT_OBJECT_0 || workerExit != 0)
    {
        std::printf("DebugLoop teardown failed: wait=%lu exit=%lu\n", wait, workerExit);
        return 1;
    }
    std::printf("PASS shim %s replay probe\n", gExpectedKind == UE_SESSION_TTD ? "TTD" : "minidump");
    // The adapter intentionally remains loaded for x64dbg's process lifetime.
    // Avoid turning this probe into a test of explicit DLL unload under the
    // Windows loader lock.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
