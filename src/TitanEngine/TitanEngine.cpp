#include <map>
#include <format>
#include <vector>
#include <mutex>
#include <queue>
#include <set>
#include <functional>
#include <atomic>

#include "TitanEngine.h"
#include "FileMap.h"

#include <DbgEng.h>
#include <delayimp.h>
#include <atlbase.h>

// https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/debugging-session-and-execution-model
// https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/introduction
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dbgeng/nf-dbgeng-idebugcontrol3-waitforevent

using namespace ATL;

static decltype(&printf) _plugin_logprintf;

static void x64dbgLog(std::string&& line)
{
    line += "\n";
    _plugin_logprintf("%s", line.c_str());
    fflush(stdout);
    OutputDebugStringA(line.c_str());
}

template<class... Args>
void logDebug(const std::format_string<Args...> fmt, Args&&... args)
{
    x64dbgLog("[dbgeng:debug] " + std::format(fmt, std::forward<Args>(args)...));
}

template<class... Args>
void logError(const std::format_string<Args...> fmt, Args&&... args)
{
    x64dbgLog("[dbgeng:error] " + std::format(fmt, std::forward<Args>(args)...));
}

static std::string Utf16ToUtf8(const wchar_t* wstr)
{
    int requiredSize = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, 0, 0, 0, 0);
    if (requiredSize <= 0)
        return {};
    std::string utf8(requiredSize - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &utf8[0], requiredSize, 0, 0);
    return utf8;
}

#define FLAG(name, description) { name, #name, description }

struct FlagInfo
{
    uint32_t value;
    const char* name;
    const char* description;

    bool operator==(const uint32_t other) const
    {
        return value == other;
    }
};

static FlagInfo cesFlags[] = {
    FLAG(DEBUG_CES_CURRENT_THREAD, "Current thread changed"),
    FLAG(DEBUG_CES_EFFECTIVE_PROCESSOR, "Effective processor changed"),
    FLAG(DEBUG_CES_BREAKPOINTS, "Breakpoints changed"),
    FLAG(DEBUG_CES_CODE_LEVEL, "Code level changed"),
    FLAG(DEBUG_CES_EXECUTION_STATUS, "Execution status changed"),
    FLAG(DEBUG_CES_ENGINE_OPTIONS, "Engine options changed"),
    FLAG(DEBUG_CES_LOG_FILE, "Log file changed"),
    FLAG(DEBUG_CES_RADIX, "Radix changed"),
    FLAG(DEBUG_CES_EVENT_FILTERS, "Event filters changed"),
    FLAG(DEBUG_CES_PROCESS_OPTIONS, "Process options changed"),
    FLAG(DEBUG_CES_EXTENSIONS, "Extensions changed"),
    FLAG(DEBUG_CES_SYSTEMS, "Systems changed"),
    FLAG(DEBUG_CES_ASSEMBLY_OPTIONS, "Assembly options changed"),
    FLAG(DEBUG_CES_EXPRESSION_SYNTAX, "Expression syntax changed"),
    FLAG(DEBUG_CES_TEXT_REPLACEMENTS, "Text replacements changed"),
};

static FlagInfo sessionFlags[] = {
    FLAG(DEBUG_SESSION_ACTIVE, "Active"),
    FLAG(DEBUG_SESSION_END_SESSION_ACTIVE_TERMINATE, "End Session Active Terminate"),
    FLAG(DEBUG_SESSION_END_SESSION_ACTIVE_DETACH, "End Session Active Detach"),
    FLAG(DEBUG_SESSION_END_SESSION_PASSIVE, "End Session Passive"),
    FLAG(DEBUG_SESSION_END, "End"),
    FLAG(DEBUG_SESSION_REBOOT, "Reboot"),
    FLAG(DEBUG_SESSION_HIBERNATE, "Hibernate"),
    FLAG(DEBUG_SESSION_FAILURE, "Failure"),
};

static FlagInfo cdsFlags[] = {
    FLAG(DEBUG_CDS_ALL, "A general change in the target has occurred."),
    FLAG(DEBUG_CDS_REGISTERS, "Registers changed"),
    FLAG(DEBUG_CDS_DATA, "Data/memory changed"),
    FLAG(DEBUG_CDS_REFRESH, "Refresh requested"),
};

static FlagInfo cssFlags[] = {
    FLAG(DEBUG_CSS_LOADS, "Symbol loads changed"),
    FLAG(DEBUG_CSS_UNLOADS, "Symbol unloads changed"),
    FLAG(DEBUG_CSS_SCOPE, "Symbol scope changed"),
    FLAG(DEBUG_CSS_PATHS, "Symbol paths changed"),
    FLAG(DEBUG_CSS_SYMBOL_OPTIONS, "Symbol options changed"),
    FLAG(DEBUG_CSS_TYPE_OPTIONS, "Type options changed"),
    FLAG(DEBUG_CSS_COLLAPSE_CHILDREN, "Collapse children changed"),
};

static FlagInfo statusFlags[] = {
    FLAG(DEBUG_STATUS_BREAK, ""),
    FLAG(DEBUG_STATUS_GO, "Target is executing normally"),
    FLAG(DEBUG_STATUS_GO_HANDLED, ""),
    FLAG(DEBUG_STATUS_GO_NOT_HANDLED, ""),
    FLAG(DEBUG_STATUS_STEP_OVER, "Target is executing a single instruction or call"),
    FLAG(DEBUG_STATUS_STEP_INTO, "Target is executing a single instruction"),
    FLAG(DEBUG_STATUS_BREAK, "Target is suspended"),
    FLAG(DEBUG_STATUS_NO_DEBUGGEE, "No debugging session is active"),
    FLAG(DEBUG_STATUS_STEP_BRANCH, "Target is executing until the next branch instruction"),
    FLAG(DEBUG_STATUS_IGNORE_EVENT, ""),
    FLAG(DEBUG_STATUS_RESTART_REQUESTED, "Target is restarting"),
    FLAG(DEBUG_STATUS_REVERSE_GO, "Target is executing backwards"),
    FLAG(DEBUG_STATUS_REVERSE_STEP_BRANCH, "Target is executing until the previous branch instruction"),
    FLAG(DEBUG_STATUS_REVERSE_STEP_OVER, "Target is executing a single instruction or call backwards"),
    FLAG(DEBUG_STATUS_REVERSE_STEP_INTO, "Target is executing a single instruction backwards"),
    FLAG(DEBUG_STATUS_OUT_OF_SYNC, "Debugger communications channel is out of sync"),
    FLAG(DEBUG_STATUS_WAIT_INPUT, "Target is awaiting input from the user"),
    FLAG(DEBUG_STATUS_TIMEOUT, "Debugger communications channel has timed out"),
};

template<size_t N>
static std::string formatSingleFlag(FlagInfo const (&flags)[N], uint32_t value)
{
    auto info = std::find(std::begin(flags), std::end(flags), value);
    if (info == std::end(flags))
    {
        return std::format("{:#x} <unknown>", value);
    }
    else
    {
        return std::format("{} ({})", info->name, info->description);
    }
}

template<size_t N>
static void logSingleFlag(const char* name, FlagInfo const (&flags)[N], uint32_t value)
{
    auto info = std::find(std::begin(flags), std::end(flags), value);
    if (info == std::end(flags))
    {
        logDebug("  {}: {:#x} <unknown>", name, value);
    }
    else
    {
        logDebug("  {}: {} ({})", name, info->name, info->description);
    }
}

template<size_t N>
static void logBitFlag(const char* name, FlagInfo const (&flags)[N], uint32_t value)
{
    logDebug("  {}: {:#x}", name, value);
    if (value == 0)
    {
        return;
    }
    for (const auto& flag : flags)
    {
        if (value & flag.value)
        {
            value &= ~flag.value;
            logDebug("    {:#x} {} ({})", flag.value, flag.name, flag.description);
        }
    }
    if (value != 0)
    {
        logDebug("    {:#x} <unknown>", value);
    }
}

#undef FLAG

struct DebugIdMap
{
    std::map<ULONG, HANDLE> processIndexToHandle;
    std::map<HANDLE, ULONG> processHandleToIndex;

    std::map<ULONG, HANDLE> threadIndexToHandle;
    std::map<HANDLE, ULONG> threadHandleToIndex;
};

enum class TitanHandleType
{
    Process,
    Thread,
};

struct TitanHandleEntry
{
    TitanHandleType type;
    ULONG dbgengId = DEBUG_ANY_ID;
    DWORD systemId = 0;
    HANDLE nativeHandle = nullptr;
    bool callerOwned = false;
    uint64_t sessionGeneration = 0;
};

enum class BreakpointKind
{
    Software,
    Hardware,
};

struct BreakpointInfo
{
    IDebugBreakpoint2* bp = nullptr;
    ULONG id = 0;
    ULONG type = 0;
    DWORD64 offset = 0;
    BreakpointKind kind = BreakpointKind::Software;
    TITANCBSOFTBP callback = nullptr;
    TITANCBHWBP hardwareCallback = nullptr;
    DWORD hardwareRegister = 0;
    bool nativePatch = false;
    bool oneShot = false;
    BYTE patchSize = 0;
    BYTE originalBytes[2] = {};
    BYTE patchBytes[2] = {};
};

struct MemoryBreakpointInfo
{
    uint64_t id = 0;
    ULONG_PTR start = 0;
    SIZE_T size = 0;
    TitanMemoryBreakpointType type = UE_MEMORY;
    bool restoreOnHit = false;
    TITANCBMEMBP callback = nullptr;
};

struct MemoryBreakpointPage
{
    DWORD originalProtect = 0;
    std::set<uint64_t> breakpointIds;
};

static PROCESS_INFORMATION gProcessInfo;
static std::map<TitanEngineVariable, bool> gEngineVariables;
static TitanBreakpointType gDefaultBreakpointType = UE_BREAKPOINT_INT3;
static std::map<TitanCustomHandler, TITANCALLBACKARG> gCustomHandlers;
static DEBUG_EVENT gFakeDebugEvent;
static std::recursive_mutex gMutexCallbackQueue;
static std::queue<std::function<bool()>> gCallbackQueue;
static std::map<HANDLE, uint64_t> gProcessPebCache;
static std::map<HANDLE, uint64_t> gProcessTebCache;
static DebugIdMap gDebugIdMap;
static std::recursive_mutex gMutexHandleRegistry;
static std::map<HANDLE, TitanHandleEntry> gHandleRegistry;
static uint64_t gSessionGeneration = 0;
static uintptr_t gNextSyntheticHandle = 1;
static std::map<ULONG, BreakpointInfo> gBreakpoints;
static std::map<DWORD, ULONG> gHardwareBreakpointIds;
static std::map<uint64_t, MemoryBreakpointInfo> gMemoryBreakpoints;
static std::map<ULONG_PTR, MemoryBreakpointPage> gMemoryBreakpointPages;
static uint64_t gNextMemoryBreakpointId = 1;
static ULONG gNextNativeBreakpointId = 0x80000000;
static std::set<ULONG64> gRetiredBreakpointAddresses;
static std::recursive_mutex gMutexPaused;
static std::atomic_bool gPaused; // this is set to true when we are not inside WaitForEvent (perhaps it should be renamed?)
static std::atomic_bool gIsDebugging;
static std::atomic<TitanSessionKind> gSessionKind = UE_SESSION_NONE;
static bool gDbgEngInitialized = false;
static bool gComInitialized = false;
static HANDLE gProcessCreatedEvent = nullptr;
static const char* gSyntheticDebugString = nullptr;
static SIZE_T gSyntheticDebugStringSize = 0;

/* DbgEng interfaces:
#define INTERFACE IDebugAdvanced4
#define INTERFACE IDebugBreakpoint3
#define INTERFACE IDebugClient9
#define INTERFACE IDebugPlmClient3
#define INTERFACE IDebugOutputStream
#define INTERFACE IDebugControl7
#define INTERFACE IDebugDataSpaces4
#define INTERFACE IDebugEventCallbacks
#define INTERFACE IDebugEventCallbacksWide
#define INTERFACE IDebugEventContextCallbacks
#define INTERFACE IDebugInputCallbacks
#define INTERFACE IDebugOutputCallbacks
#define INTERFACE IDebugOutputCallbacksWide
#define INTERFACE IDebugOutputCallbacks2
#define INTERFACE IDebugRegisters2
#define INTERFACE IDebugSymbolGroup2
#define INTERFACE IDebugSymbols5
#define INTERFACE IDebugSystemObjects4
*/

static IDebugClient5* gDebugClient = nullptr;

template<class T>
static HRESULT debugClientInterface(T* i)
{
    return gDebugClient->QueryInterface(__uuidof(T), (PVOID*)i);
};

static IDebugControl7* gDebugControl = nullptr;
static IDebugDataSpaces4* gDebugDataSpaces = nullptr;
static IDebugRegisters2* gDebugRegisters = nullptr;
static IDebugSymbols5* gDebugSymbols = nullptr;
static IDebugSystemObjects4* gDebugSystemObjects = nullptr;
static DWORD gDebugThreadId = 0;
static std::map<ULONG, TITANCBSTEP> gStepCallbacks;
static std::map<ULONG, DWORD> gStepThreadSystemIds;
static std::map<ULONG, std::function<void()>> gInternalStepCallbacks;
static std::map<ULONG, std::vector<HANDLE>> gInternalStepSuspendedThreads;
static std::map<ULONG, ULONG> gStepStatuses;
static ULONG gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
static std::atomic<DWORD> gNextContinueStatus = DBG_CONTINUE;
static bool gExpectSystemBreakpoint = false;
static TITANCALLBACK gAttachCallback = nullptr;

static bool IsDebugThread()
{
    return gDebugThreadId == GetCurrentThreadId();
}

struct PauseLock
{
    PauseLock()
    {
        if (!(gPaused && gIsDebugging))
        {
            mLocked = false;
        }
        else
        {
            gMutexPaused.lock();
            mLocked = true;
        }
    }

    ~PauseLock()
    {
        if (mLocked)
        {
            gMutexPaused.unlock();
        }
    }

    PauseLock(const PauseLock&) = delete;
    PauseLock& operator=(const PauseLock&) = delete;
    PauseLock(PauseLock&&) = delete;
    PauseLock& operator=(PauseLock&&) = delete;

    operator bool() const
    {
        return mLocked;
    }

private:
    bool mLocked = false;
};

static void queueCallback(std::function<bool()> work)
{
    std::lock_guard lock(gMutexCallbackQueue);
    gCallbackQueue.push(std::move(work));
}

static void invokeCustomHandler(TitanCustomHandler id, const void* argument)
{
    auto found = gCustomHandlers.find(id);
    if (found != gCustomHandlers.end() && found->second)
        found->second(argument);
}

static void beginDebugEvent(bool unhandledException)
{
    gNextContinueStatus = unhandledException ? DBG_EXCEPTION_NOT_HANDLED : DBG_CONTINUE;
    invokeCustomHandler(UE_CH_DEBUGEVENT, &gFakeDebugEvent);
}

static void finishDebugEvent(bool unhandledException)
{
    if (gNextExecutionStatus == DEBUG_STATUS_NO_CHANGE)
    {
        if (gSessionKind == UE_SESSION_MINIDUMP)
            gNextExecutionStatus = DEBUG_STATUS_BREAK;
        else if (!gStepStatuses.empty())
            gNextExecutionStatus = gStepStatuses.begin()->second;
        else
            gNextExecutionStatus = unhandledException && gNextContinueStatus == DBG_EXCEPTION_NOT_HANDLED
                                       ? DEBUG_STATUS_GO_NOT_HANDLED
                                       : DEBUG_STATUS_GO_HANDLED;
    }
}

static void dispatchDebugEvent(TitanCustomHandler id, const void* argument, bool unhandledException = false)
{
    beginDebugEvent(unhandledException);
    invokeCustomHandler(id, argument);
    finishDebugEvent(unhandledException);
}

static void setCurrentInstructionPointer(ULONG_PTR address);
static bool writeNativeBreakpointBytes(const BreakpointInfo& info, bool install);

static void dispatchSystemBreakpoint(const void* argument)
{
    beginDebugEvent(false);
    if (gAttachCallback)
    {
        const auto callback = gAttachCallback;
        gAttachCallback = nullptr;
        callback();
    }
    else
    {
        invokeCustomHandler(UE_CH_SYSTEMBREAKPOINT, argument);
    }
    finishDebugEvent(false);
}

static DWORD debugProcessId()
{
    if (!gDebugSystemObjects)
        return 0;
    ULONG id = 0;
    if (FAILED(gDebugSystemObjects->GetCurrentProcessSystemId(&id)))
        return 0;
    return id;
}

static DWORD debugThreadId()
{
    if (!gDebugSystemObjects)
        return 0;
    ULONG id = 0;
    if (FAILED(gDebugSystemObjects->GetCurrentThreadSystemId(&id)))
        return 0;
    return id;
}

static void setFakeDebugEvent(const CREATE_PROCESS_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = CREATE_PROCESS_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.CreateProcessInfo = info;
}

static void setFakeDebugEvent(const EXIT_PROCESS_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = EXIT_PROCESS_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.ExitProcess = info;
}

static void setFakeDebugEvent(const LOAD_DLL_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = LOAD_DLL_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.LoadDll = info;
}

static void setFakeDebugEvent(const UNLOAD_DLL_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = UNLOAD_DLL_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.UnloadDll = info;
}

static void setFakeDebugEvent(const EXCEPTION_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = EXCEPTION_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.Exception = info;
}

static void setFakeDebugEvent(const CREATE_THREAD_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = CREATE_THREAD_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.CreateThread = info;
}

static void setFakeDebugEvent(const EXIT_THREAD_DEBUG_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = EXIT_THREAD_DEBUG_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.ExitThread = info;
}

static void setFakeDebugEvent(const OUTPUT_DEBUG_STRING_INFO& info)
{
    gFakeDebugEvent = {};
    gFakeDebugEvent.dwDebugEventCode = OUTPUT_DEBUG_STRING_EVENT;
    gFakeDebugEvent.dwProcessId = debugProcessId();
    gFakeDebugEvent.dwThreadId = debugThreadId();
    gFakeDebugEvent.u.DebugString = info;
}

static void registerTitanHandle(HANDLE handle, TitanHandleType type, ULONG dbgengId, DWORD systemId, bool callerOwned, HANDLE nativeHandle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return;
    std::lock_guard lock(gMutexHandleRegistry);
    gHandleRegistry.insert_or_assign(handle, TitanHandleEntry { type, dbgengId, systemId, nativeHandle, callerOwned, gSessionGeneration });
}

static void registerTitanHandle(HANDLE handle, TitanHandleType type, ULONG dbgengId, DWORD systemId, bool callerOwned)
{
    registerTitanHandle(handle, type, dbgengId, systemId, callerOwned, handle);
}

static HANDLE createSyntheticTitanHandle(TitanHandleType type, ULONG dbgengId, DWORD systemId, bool callerOwned)
{
    const auto serial = gNextSyntheticHandle++;
#ifdef _WIN64
    const auto value = 0xFFFF800000000000ull | ((serial & 0x00007FFFFFFFFFFFull) << 4) | 3;
#else
    const auto value = 0xF0000000u | ((serial & 0x00FFFFFFu) << 4) | 3;
#endif
    const auto handle = (HANDLE)(uintptr_t)value;
    registerTitanHandle(handle, type, dbgengId, systemId, callerOwned, nullptr);
    return handle;
}

static void unregisterTitanHandle(HANDLE handle)
{
    std::lock_guard lock(gMutexHandleRegistry);
    gHandleRegistry.erase(handle);
}

static HANDLE registeredNativeHandle(HANDLE handle, TitanHandleType expectedType)
{
    std::lock_guard lock(gMutexHandleRegistry);
    const auto found = gHandleRegistry.find(handle);
    if (found == gHandleRegistry.end() || found->second.type != expectedType || !found->second.nativeHandle)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    return found->second.nativeHandle;
}

static void closeCallerOwnedTitanHandles()
{
    std::lock_guard lock(gMutexHandleRegistry);
    for (const auto& [handle, entry] : gHandleRegistry)
    {
        if (entry.callerOwned && entry.nativeHandle)
            CloseHandle(entry.nativeHandle);
    }
    gHandleRegistry.clear();
}

static SIZE_T targetPageSize()
{
    static const SIZE_T pageSize = []
    {
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        return (SIZE_T)info.dwPageSize;
    }();
    return pageSize;
}

static ULONG_PTR pageAddress(ULONG_PTR address)
{
    return address & ~(targetPageSize() - 1);
}

static HANDLE activeProcessHandle()
{
    if (!gDebugSystemObjects)
        return nullptr;
    ULONG processIndex = 0;
    if (FAILED(gDebugSystemObjects->GetCurrentProcessId(&processIndex)))
        return nullptr;
    auto found = gDebugIdMap.processIndexToHandle.find(processIndex);
    return found == gDebugIdMap.processIndexToHandle.end() ? nullptr : found->second;
}

static bool protectMemoryBreakpointPage(ULONG_PTR page, bool guarded)
{
    auto found = gMemoryBreakpointPages.find(page);
    if (found == gMemoryBreakpointPages.end())
        return false;
    auto process = activeProcessHandle();
    if (!process)
        return false;
    DWORD ignored = 0;
    const DWORD protection = guarded ? found->second.originalProtect | PAGE_GUARD
                                     : found->second.originalProtect;
    if (!VirtualProtectEx(process, (LPVOID)page, targetPageSize(), protection, &ignored))
    {
        logError("VirtualProtectEx({:#x}, {:#x}) failed for memory breakpoint: {:#x}",
                 page, protection, (uint32_t)GetLastError());
        return false;
    }
    return true;
}

static bool removeMemoryBreakpointById(uint64_t id)
{
    auto breakpoint = gMemoryBreakpoints.find(id);
    if (breakpoint == gMemoryBreakpoints.end())
        return false;

    const auto startPage = pageAddress(breakpoint->second.start);
    const auto endPage = pageAddress(breakpoint->second.start + breakpoint->second.size - 1);
    bool result = true;
    for (auto page = startPage;; page += targetPageSize())
    {
        auto found = gMemoryBreakpointPages.find(page);
        if (found != gMemoryBreakpointPages.end())
        {
            found->second.breakpointIds.erase(id);
            if (found->second.breakpointIds.empty())
            {
                result = protectMemoryBreakpointPage(page, false) && result;
                gMemoryBreakpointPages.erase(found);
            }
        }
        if (page == endPage)
            break;
    }
    gMemoryBreakpoints.erase(breakpoint);
    return result;
}

static bool removeAllMemoryBreakpoints()
{
    bool result = true;
    while (!gMemoryBreakpoints.empty())
        result = removeMemoryBreakpointById(gMemoryBreakpoints.begin()->first) && result;
    return result;
}

static bool memoryAccessMatches(TitanMemoryBreakpointType type, ULONG_PTR accessType)
{
    switch (type)
    {
    case UE_MEMORY: return true;
    case UE_MEMORY_READ: return accessType == 0;
    case UE_MEMORY_WRITE: return accessType == 1;
    case UE_MEMORY_EXECUTE: return accessType == 8;
    default: return false;
    }
}

static void scheduleInternalStep(std::function<void()> callback)
{
    ULONG threadIndex = 0;
    if (FAILED(gDebugSystemObjects->GetCurrentThreadId(&threadIndex)))
    {
        logError("Failed to get current thread for internal step");
        return;
    }

    auto existingInternalStep = gInternalStepCallbacks.find(threadIndex);
    if (existingInternalStep != gInternalStepCallbacks.end())
    {
        auto previous = std::move(existingInternalStep->second);
        existingInternalStep->second = [previous = std::move(previous), callback = std::move(callback)]
        {
            previous();
            callback();
        };
        return;
    }

    TITANCBSTEP userStep = nullptr;
    auto existingUserStep = gStepCallbacks.find(threadIndex);
    if (existingUserStep != gStepCallbacks.end())
    {
        userStep = existingUserStep->second;
        gStepCallbacks.erase(existingUserStep);
        gStepThreadSystemIds.erase(threadIndex);
    }

    // DbgEng's STEP_INTO status does not mark the current guard-page
    // exception handled. Set TF ourselves and continue the guard exception as
    // handled; the resulting single-step is consumed below.
    auto thread = gDebugIdMap.threadIndexToHandle.find(threadIndex);
    if (thread == gDebugIdMap.threadIndexToHandle.end())
    {
        logError("No thread handle for internal step");
        return;
    }
    // PAGE_GUARD is process-wide. Keep all other target threads suspended
    // while this thread executes the one unguarded instruction, otherwise a
    // racing thread can access the page before the guard is restored.
    auto& suspendedThreads = gInternalStepSuspendedThreads[threadIndex];
    for (const auto& [otherIndex, otherHandle] : gDebugIdMap.threadIndexToHandle)
    {
        if (otherIndex != threadIndex && TitanSuspendThread(otherHandle) != (DWORD)-1)
            suspendedThreads.push_back(otherHandle);
    }

    const auto flags = GetContextDataEx(thread->second, UE_CFLAGS);
    if (!SetContextDataEx(thread->second, UE_CFLAGS, flags | 0x100))
    {
        logError("Failed to set trap flag for internal step");
        for (const auto suspended : suspendedThreads)
            TitanResumeThread(suspended);
        gInternalStepSuspendedThreads.erase(threadIndex);
        return;
    }

    gInternalStepCallbacks[threadIndex] = [callback = std::move(callback), userStep]
    {
        callback();
        if (userStep)
            userStep();
    };
    gStepStatuses[threadIndex] = DEBUG_STATUS_GO_HANDLED;
}

static void scheduleMemoryBreakpointRearm(ULONG_PTR page)
{
    scheduleInternalStep([page]
    {
        if (gMemoryBreakpointPages.contains(page))
            protectMemoryBreakpointPage(page, true);
    });
}

static bool completeInternalStep(ULONG threadIndex)
{
    auto internalStep = gInternalStepCallbacks.find(threadIndex);
    if (internalStep == gInternalStepCallbacks.end())
        return false;

    auto thread = gDebugIdMap.threadIndexToHandle.find(threadIndex);
    if (thread != gDebugIdMap.threadIndexToHandle.end())
    {
        const auto flags = GetContextDataEx(thread->second, UE_CFLAGS);
        SetContextDataEx(thread->second, UE_CFLAGS, flags & ~ULONG_PTR(0x100));
    }
    auto callback = std::move(internalStep->second);
    gInternalStepCallbacks.erase(internalStep);
    gStepStatuses.erase(threadIndex);
    gNextExecutionStatus = DEBUG_STATUS_GO_HANDLED;
    callback();
    auto suspended = gInternalStepSuspendedThreads.find(threadIndex);
    if (suspended != gInternalStepSuspendedThreads.end())
    {
        for (const auto handle : suspended->second)
            TitanResumeThread(handle);
        gInternalStepSuspendedThreads.erase(suspended);
    }
    return true;
}

static bool dispatchMemoryBreakpointException(const EXCEPTION_DEBUG_INFO& info)
{
    if (info.ExceptionRecord.NumberParameters < 2)
        return false;

    const auto accessType = info.ExceptionRecord.ExceptionInformation[0];
    const auto accessAddress = info.ExceptionRecord.ExceptionInformation[1];
    const auto page = pageAddress(accessAddress);
    auto pageInfo = gMemoryBreakpointPages.find(page);
    if (pageInfo == gMemoryBreakpointPages.end())
        return false;

    MemoryBreakpointInfo hit = {};
    for (const auto id : pageInfo->second.breakpointIds)
    {
        auto candidate = gMemoryBreakpoints.find(id);
        if (candidate == gMemoryBreakpoints.end())
            continue;
        const auto& bp = candidate->second;
        if (accessAddress >= bp.start && accessAddress - bp.start < bp.size &&
            memoryAccessMatches(bp.type, accessType))
        {
            hit = bp;
            break;
        }
    }

    setFakeDebugEvent(info);
    beginDebugEvent(false);
    if (hit.id)
    {
        if (!hit.restoreOnHit)
            removeMemoryBreakpointById(hit.id);
        if (hit.callback)
            hit.callback((const void*)accessAddress);
    }

    // PAGE_GUARD is cleared by Windows before delivery. If the callback left
    // any breakpoint on this page, execute the faulting instruction once and
    // restore the guard before normal execution or a user step continues.
    if (gMemoryBreakpointPages.contains(page))
    {
        // Explicitly expose the faulting page. Windows normally clears
        // PAGE_GUARD before delivery, but doing this here makes cross-page
        // instructions deterministic across DbgEng/runtime versions.
        protectMemoryBreakpointPage(page, false);
        scheduleMemoryBreakpointRearm(page);
    }
    finishDebugEvent(false);
    return true;
}

/* Event callback class. Some notes about how events are delivered:
ChangeEngineState:
- Called during initialization (DEBUG_CES_EVENT_FILTERS, DEBUG_CES_ENGINE_OPTIONS)
- Called from inside APIs (gDebugClient->CreateProcessWide, gDebugControl->SetExecutionStatus, gDebugControl->AddBreakpoint2)
- Also delivered from DebugLoop (WaitForEvent)
SessionStatus: delivered with APC
ChangeDebuggeeState: delivered in DebugLoop, AddBreakpoint2
DEBUG_CDS_REFRESH after setting breakpoint
CreateProcess: delivered with APC
ChangeEngineState: delivered in DebugLoop
LoadModule: delivered with APC
Breakpoint: delivered with APC
ChangeSymbolState: DebugLoop (WaitForEvent)
*/
class DebugEventCallbacks : public IDebugEventCallbacksWide
{
private:
    ULONG mRefCount = 1;
    DWORD mEventThreadId = GetCurrentThreadId();
    ULONG64 mLastCurrentThread = DEBUG_ANY_ID;

public:
    // IUnknown methods
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&mRefCount); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG count = InterlockedDecrement(&mRefCount);
        if (count == 0)
            delete this;
        return count;
    }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDebugEventCallbacksWide))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    // IDebugEventCallbacks methods
    STDMETHOD(GetInterestMask)(PULONG Mask) override
    {
        *Mask = DEBUG_EVENT_BREAKPOINT | DEBUG_EVENT_EXCEPTION |
                DEBUG_EVENT_CREATE_THREAD | DEBUG_EVENT_EXIT_THREAD |
                DEBUG_EVENT_CREATE_PROCESS | DEBUG_EVENT_EXIT_PROCESS |
                DEBUG_EVENT_LOAD_MODULE | DEBUG_EVENT_UNLOAD_MODULE |
                DEBUG_EVENT_SYSTEM_ERROR | DEBUG_EVENT_SESSION_STATUS |
                DEBUG_EVENT_CHANGE_DEBUGGEE_STATE | DEBUG_EVENT_CHANGE_ENGINE_STATE |
                DEBUG_EVENT_CHANGE_SYMBOL_STATE | DEBUG_EVENT_SERVICE_EXCEPTION;
        return S_OK;
    }

    STDMETHOD(Breakpoint)(PDEBUG_BREAKPOINT2 Bp) override
    {
        logDebug("[{}] Breakpoint hit", __func__);
        ULONG64 offset = 0;
        auto hr = Bp->GetOffset(&offset);
        if (FAILED(hr))
        {
            logError("Failed to get breakpoint offset: {:#x}", (uint32_t)hr);
            offset = -1;
        }
        logDebug("  Offset: {:#x}", offset);
        ULONG breakType = 0;
        ULONG procType = 0;
        hr = Bp->GetType(&breakType, &procType);
        if (FAILED(hr))
        {
            logError("Failed to get breakpoint type: {:#x}", (uint32_t)hr);
            breakType = 0;
            procType = 0;
        }
        logDebug("  BreakType: {:#x}", breakType);
        logDebug("  ProcType: {:#x}", procType);
        ULONG id = 0;
        hr = Bp->GetId(&id);
        if (FAILED(hr))
        {
            logError("Failed to get breakpoint id: {:#x}", (uint32_t)hr);
            id = -1;
        }
        logDebug("  Id: {}", id);
        const auto eventProcessId = debugProcessId();
        const auto eventThreadId = debugThreadId();

        auto work = [=]
        {
            auto itr = gBreakpoints.find(id);
            if (itr == gBreakpoints.end())
            {
                EXCEPTION_DEBUG_INFO info = {};
                info.ExceptionRecord.ExceptionCode = EXCEPTION_BREAKPOINT;
                info.ExceptionRecord.ExceptionAddress = (PVOID)offset;
                setFakeDebugEvent(info);
                gFakeDebugEvent.dwProcessId = eventProcessId;
                gFakeDebugEvent.dwThreadId = eventThreadId;
                if (gExpectSystemBreakpoint)
                {
                    gExpectSystemBreakpoint = false;
                    dispatchSystemBreakpoint(&info);
                }
                else
                {
                    logError("Breakpoint id {} not found in map", id);
                    dispatchDebugEvent(UE_CH_UNHANDLEDEXCEPTION, &info, true);
                }
                return true;
            }

            // Copy callback state because x64dbg is allowed to delete the
            // breakpoint from inside its callback.
            const auto bpInfo = itr->second;
            EXCEPTION_DEBUG_INFO info = {};
            info.ExceptionRecord.ExceptionCode = bpInfo.kind == BreakpointKind::Hardware
                                                     ? EXCEPTION_SINGLE_STEP
                                                     : EXCEPTION_BREAKPOINT;
            info.ExceptionRecord.ExceptionAddress = (PVOID)offset;
            setFakeDebugEvent(info);
            gFakeDebugEvent.dwProcessId = eventProcessId;
            gFakeDebugEvent.dwThreadId = eventThreadId;

            beginDebugEvent(false);
            if (bpInfo.kind == BreakpointKind::Hardware)
            {
                if (bpInfo.hardwareCallback)
                    bpInfo.hardwareCallback((const void*)(ULONG_PTR)bpInfo.offset);
            }
            else if (bpInfo.callback)
            {
                bpInfo.callback();
            }
            if (bpInfo.kind == BreakpointKind::Software && bpInfo.oneShot)
            {
                auto current = gBreakpoints.find(id);
                if (current != gBreakpoints.end() &&
                    current->second.kind == BreakpointKind::Software &&
                    current->second.offset == bpInfo.offset)
                    gBreakpoints.erase(current);
            }
            finishDebugEvent(false);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(Exception)(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) override
    {
        logDebug("[{}] Exception thrown", __func__);
        logDebug("  FirstChance: {}", FirstChance);
        logDebug("  ExceptionCode: {:#x}", Exception->ExceptionCode);
        logDebug("  ExceptionFlags: {:#x}", Exception->ExceptionFlags);
        logDebug("  ExceptionRecord: {:#x}", Exception->ExceptionRecord);
        logDebug("  ExceptionAddress: {:#x}", Exception->ExceptionAddress);
        logDebug("  NumberParameters: {:#x}", Exception->NumberParameters);
        for (int i = 0; i < Exception->NumberParameters; i++)
        {
            logDebug("    ExceptionInformation[{}]: {:#x}", i, Exception->ExceptionInformation[i]);
        }

        if ((Exception->ExceptionCode == DBG_PRINTEXCEPTION_C || Exception->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) &&
            Exception->NumberParameters >= 2)
        {
            OUTPUT_DEBUG_STRING_INFO output = {};
            output.lpDebugStringData = (LPSTR)(ULONG_PTR)Exception->ExceptionInformation[1];
            output.fUnicode = Exception->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C;
            output.nDebugStringLength = (WORD)std::min<ULONG64>(Exception->ExceptionInformation[0], USHRT_MAX);
            queueCallback([output]
            {
                setFakeDebugEvent(output);
                dispatchDebugEvent(UE_CH_OUTPUTDEBUGSTRING, &output);
                return true;
            });
            return DEBUG_STATUS_BREAK;
        }

        const auto eventProcessId = debugProcessId();
        const auto eventThreadId = debugThreadId();
        EXCEPTION_DEBUG_INFO info = {};
        info.dwFirstChance = FirstChance;
        info.ExceptionRecord.ExceptionAddress = (PVOID)Exception->ExceptionAddress;
        info.ExceptionRecord.ExceptionCode = Exception->ExceptionCode;
        info.ExceptionRecord.ExceptionFlags = Exception->ExceptionFlags;
        info.ExceptionRecord.NumberParameters = Exception->NumberParameters;
        for (int i = 0; i < Exception->NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
        {
            info.ExceptionRecord.ExceptionInformation[i] = (ULONG_PTR)Exception->ExceptionInformation[i];
        }

        auto work = [=]
        {
            setFakeDebugEvent(info);
            gFakeDebugEvent.dwProcessId = eventProcessId;
            gFakeDebugEvent.dwThreadId = eventThreadId;
            if (info.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP)
            {
                ULONG threadIndex = 0;
                if (SUCCEEDED(gDebugSystemObjects->GetCurrentThreadId(&threadIndex)) &&
                    completeInternalStep(threadIndex))
                {
                    return true;
                }
            }
            if ((info.ExceptionRecord.ExceptionCode == STATUS_GUARD_PAGE_VIOLATION ||
                 info.ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) &&
                dispatchMemoryBreakpointException(info))
            {
                return true;
            }
            if (info.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && gExpectSystemBreakpoint)
            {
                gExpectSystemBreakpoint = false;
                dispatchSystemBreakpoint(&info);
            }
            else if (info.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT ||
                     info.ExceptionRecord.ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)
            {
                const auto address = (ULONG64)(ULONG_PTR)info.ExceptionRecord.ExceptionAddress;
                auto nativeBreakpoint = std::find_if(gBreakpoints.begin(), gBreakpoints.end(), [address](const auto& entry)
                {
                    const auto& bp = entry.second;
                    const bool longInt3Address = bp.patchSize == 2 && bp.patchBytes[0] == 0xCD &&
                                                 bp.patchBytes[1] == 0x03 && bp.offset + 1 == address;
                    return bp.nativePatch && (bp.offset == address || longInt3Address);
                });
                if (nativeBreakpoint != gBreakpoints.end())
                {
                    const auto id = nativeBreakpoint->first;
                    const auto bpInfo = nativeBreakpoint->second;
                    const auto breakpointAddress = bpInfo.offset;
                    if (!writeNativeBreakpointBytes(bpInfo, false))
                    {
                        logError("Failed to restore native breakpoint at {:#x}", breakpointAddress);
                        return false;
                    }
                    setCurrentInstructionPointer((ULONG_PTR)breakpointAddress);
                    if (bpInfo.kind == BreakpointKind::Hardware)
                        gFakeDebugEvent.u.Exception.ExceptionRecord.ExceptionCode = EXCEPTION_SINGLE_STEP;
                    if (bpInfo.oneShot)
                    {
                        gRetiredBreakpointAddresses.insert(breakpointAddress);
                        gBreakpoints.erase(id);
                    }

                    beginDebugEvent(false);
                    if (bpInfo.kind == BreakpointKind::Hardware)
                    {
                        if (bpInfo.hardwareCallback)
                            bpInfo.hardwareCallback((const void*)(ULONG_PTR)breakpointAddress);
                    }
                    else if (bpInfo.callback)
                    {
                        bpInfo.callback();
                    }
                    if (!bpInfo.oneShot && gBreakpoints.contains(id))
                    {
                        scheduleInternalStep([id]
                        {
                            auto current = gBreakpoints.find(id);
                            if (current != gBreakpoints.end() && !writeNativeBreakpointBytes(current->second, true))
                                logError("Failed to rearm native breakpoint at {:#x}", current->second.offset);
                        });
                    }
                    finishDebugEvent(false);
                }
                else if (gRetiredBreakpointAddresses.contains(address) ||
                         (address && gRetiredBreakpointAddresses.contains(address - 1)))
                {
                    logDebug("Suppressing stale breakpoint exception at {:#x}", address);
                    beginDebugEvent(false);
                    finishDebugEvent(false);
                }
                else
                {
                    dispatchDebugEvent(UE_CH_UNHANDLEDEXCEPTION, &info, true);
                }
            }
            else
            {
                dispatchDebugEvent(UE_CH_UNHANDLEDEXCEPTION, &info, true);
            }
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateThread)(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override
    {
        logDebug("[{}] Thread created", __func__);
        logDebug("  Handle: {:#x}", Handle);
        logDebug("  DataOffset: {:#x}", DataOffset);
        logDebug("  StartOffset: {:#x}", StartOffset);

        auto work = [=]
        {
            CREATE_THREAD_DEBUG_INFO info = {};
            // NOTE: the hThread is set based on the Handle
            info.hThread = (HANDLE)Handle;
            // NOTE: the lpThreadLocalBase is set based on the DataOffset
            info.lpThreadLocalBase = (LPVOID)DataOffset;
            // NOTE: the lpStartAddress is set based on the StartOffset
            info.lpStartAddress = (LPTHREAD_START_ROUTINE)StartOffset;
            // NOTE: Used by GetTEBLocation
            gProcessTebCache[info.hThread] = DataOffset;
            HRESULT hr = {};
            if (gSessionKind == UE_SESSION_LIVE)
            {
                ULONG threadIndex = 0;
                hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event thread index");
                    threadIndex = -1;
                }
                gDebugIdMap.threadHandleToIndex[info.hThread] = threadIndex;
                gDebugIdMap.threadIndexToHandle[threadIndex] = info.hThread;
                registerTitanHandle(info.hThread, TitanHandleType::Thread, threadIndex, debugThreadId(), false);
                // NOTE: sanity check
                ULONG handleIndex = 0;
                hr = gDebugSystemObjects->GetThreadIdByHandle(Handle, &handleIndex);
                if (FAILED(hr))
                    logError("Failed to get event thread index by handle");
            }
            setFakeDebugEvent(info);
            if (gSessionKind != UE_SESSION_LIVE)
            {
                std::lock_guard lock(gMutexHandleRegistry);
                const auto found = gHandleRegistry.find(info.hThread);
                if (found != gHandleRegistry.end())
                    gFakeDebugEvent.dwThreadId = found->second.systemId;
            }
            dispatchDebugEvent(UE_CH_CREATETHREAD, &info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(ExitThread)(ULONG ExitCode) override
    {
        logDebug("[{}] Thread exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);

        auto work = [=]
        {
            EXIT_THREAD_DEBUG_INFO info = {};
            info.dwExitCode = ExitCode;
            setFakeDebugEvent(info);
            ULONG threadIndex = DEBUG_ANY_ID;
            gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
            HANDLE threadHandle = nullptr;
            auto handle = gDebugIdMap.threadIndexToHandle.find(threadIndex);
            if (handle != gDebugIdMap.threadIndexToHandle.end())
                threadHandle = handle->second;

            dispatchDebugEvent(UE_CH_EXITTHREAD, &info);

            if (threadHandle)
            {
                gProcessTebCache.erase(threadHandle);
                gDebugIdMap.threadHandleToIndex.erase(threadHandle);
                gDebugIdMap.threadIndexToHandle.erase(threadIndex);
                unregisterTitanHandle(threadHandle);
            }
            gStepCallbacks.erase(threadIndex);
            gStepThreadSystemIds.erase(threadIndex);
            gInternalStepCallbacks.erase(threadIndex);
            gInternalStepSuspendedThreads.erase(threadIndex);
            gStepStatuses.erase(threadIndex);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateProcess)(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp, ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset, ULONG64 StartOffset) override
    {
        logDebug("dwThreadId: {:#x}", GetCurrentThreadId());
        logDebug("[{}] Process created", __func__);
        logDebug("  ImageFileHandle: {:#x}", ImageFileHandle);
        logDebug("  Handle: {:#x}", Handle);
        logDebug("  BaseOffset: {:#x}", BaseOffset);
        logDebug("  ModuleSize: {:#x}", ModuleSize);
        logDebug("  ModuleName: {}", ModuleName ? Utf16ToUtf8(ModuleName) : "<unknown>");
        logDebug("  ImageName: {}", ImageName ? Utf16ToUtf8(ImageName) : "<unknown>");
        logDebug("  CheckSum: {:#x}", CheckSum);
        logDebug("  TimeDateStamp: {:#x}", TimeDateStamp);
        logDebug("  InitialThreadHandle: {:#x}", InitialThreadHandle);
        logDebug("  ThreadDataOffset: {:#x}", ThreadDataOffset);
        logDebug("  StartOffset: {:#x}", StartOffset);

        if (gProcessInfo.dwProcessId == 0)
        {
            gProcessInfo.hProcess = (HANDLE)Handle;
            gProcessInfo.hThread = (HANDLE)InitialThreadHandle;
            gProcessInfo.dwProcessId = debugProcessId();
            gProcessInfo.dwThreadId = debugThreadId();
            if (gProcessCreatedEvent)
                SetEvent(gProcessCreatedEvent);
        }

        auto work = [=]
        {
            CREATE_PROCESS_DEBUG_INFO info = {};
            // NOTE: the file handle is used to get the image name (GetFileNameFromHandle and GetFileNameFromProcessHandle with hProcess on fallback)
            info.hFile = (HANDLE)ImageFileHandle;
            // NOTE: the fdProcessInfo (returned from InitDebugW) handles are reassigned here by x64dbg
            info.hProcess = (HANDLE)Handle;
            // NOTE: The hActiveThread is set based on the hThread
            // A thread is also implicitly created from this handle (DuplicateHandle)
            info.hThread = (HANDLE)InitialThreadHandle;
            // NOTE: the pDebuggedBase is set based on the lpBaseOfImage
            info.lpBaseOfImage = (LPVOID)BaseOffset;
            info.dwDebugInfoFileOffset = 0;
            info.nDebugInfoSize = 0;
            // NOTE: used to create the initial thread
            info.lpThreadLocalBase = (LPVOID)ThreadDataOffset;
            // NOTE: used to create the initial thread
            info.lpStartAddress = (LPTHREAD_START_ROUTINE)StartOffset;
            // NOTE: unused by x64dbg (since winapi doesn't set it reliably)
            info.lpImageName = nullptr;
            info.fUnicode = 0;

            // NOTE: Used by GetTEBLocation
            gProcessTebCache[info.hThread] = ThreadDataOffset;

            HRESULT hr = {};
            {
                ULONG processIndex = 0;
                hr = gDebugSystemObjects->GetCurrentProcessId(&processIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event process index");
                    processIndex = -1;
                }
                gDebugIdMap.processHandleToIndex[info.hProcess] = processIndex;
                gDebugIdMap.processIndexToHandle[processIndex] = info.hProcess;
                if (gSessionKind == UE_SESSION_LIVE)
                {
                    registerTitanHandle(info.hProcess, TitanHandleType::Process, processIndex, debugProcessId(), false);

                    // NOTE: sanity check
                    ULONG handleIndex = 0;
                    hr = gDebugSystemObjects->GetProcessIdByHandle(Handle, &handleIndex);
                    if (FAILED(hr))
                        logError("Failed to get event process index by handle");
                    else if (handleIndex != processIndex)
                        logError("Process index mismatch: current {}, handle {}", processIndex, handleIndex);
                }
            }
            {
                ULONG threadIndex = 0;
                hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event thread index");
                    threadIndex = -1;
                }
                gDebugIdMap.threadHandleToIndex[info.hThread] = threadIndex;
                gDebugIdMap.threadIndexToHandle[threadIndex] = info.hThread;
                if (gSessionKind == UE_SESSION_LIVE)
                {
                    registerTitanHandle(info.hThread, TitanHandleType::Thread, threadIndex, debugThreadId(), false);

                    // NOTE: sanity check
                    ULONG handleIndex = 0;
                    hr = gDebugSystemObjects->GetThreadIdByHandle(InitialThreadHandle, &handleIndex);
                    if (FAILED(hr))
                        logError("Failed to get event thread index by handle");
                }
            }

            // NOTE: Used by GetPEBLocation
            ULONG64 peb = 0;
            hr = gDebugSystemObjects->GetCurrentProcessPeb(&peb);
            if (FAILED(hr))
            {
                logError("Failed to get PEB from GetCurrentProcessPeb: {:#x}", (uint32_t)hr);
                peb = 0;
            }
            gProcessPebCache[info.hProcess] = peb;

            setFakeDebugEvent(info);
            dispatchDebugEvent(UE_CH_CREATEPROCESS, &info);

            // ImageFileHandle is borrowed from DbgEng; the engine owns it.
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(ExitProcess)(ULONG ExitCode) override
    {
        logDebug("[{}] Process exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);

        auto work = [=]
        {
            EXIT_PROCESS_DEBUG_INFO info = {};
            info.dwExitCode = ExitCode;
            setFakeDebugEvent(info);
            ULONG processIndex = DEBUG_ANY_ID;
            gDebugSystemObjects->GetCurrentProcessId(&processIndex);
            HANDLE processHandle = nullptr;
            auto handle = gDebugIdMap.processIndexToHandle.find(processIndex);
            if (handle != gDebugIdMap.processIndexToHandle.end())
                processHandle = handle->second;

            dispatchDebugEvent(UE_CH_EXITPROCESS, &info);

            if (processHandle)
            {
                gProcessPebCache.erase(processHandle);
                gDebugIdMap.processHandleToIndex.erase(processHandle);
                gDebugIdMap.processIndexToHandle.erase(processIndex);
                unregisterTitanHandle(processHandle);
            }
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(LoadModule)(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp) override
    {
        logDebug("[{}] Module loaded", __func__);
        logDebug("  ImageFileHandle: {:#x}", ImageFileHandle);
        logDebug("  BaseOffset: {:#x}", BaseOffset);
        logDebug("  ModuleSize: {:#x}", ModuleSize);
        logDebug("  ModuleName: {}", ModuleName ? Utf16ToUtf8(ModuleName) : "<unknown>");
        logDebug("  ImageName: {}", ImageName ? Utf16ToUtf8(ImageName) : "<unknown>");
        logDebug("  CheckSum: {:#x}", CheckSum);
        logDebug("  TimeDateStamp: {:#x}", TimeDateStamp);

        auto work = [=]
        {
            LOAD_DLL_DEBUG_INFO info = {};
            info.hFile = (HANDLE)ImageFileHandle;
            info.lpBaseOfDll = (LPVOID)BaseOffset;

            // NOTE: unused by x64dbg
            info.dwDebugInfoFileOffset = 0;
            info.nDebugInfoSize = 0;
            info.lpImageName = nullptr;
            info.fUnicode = 1;

            setFakeDebugEvent(info);
            dispatchDebugEvent(UE_CH_LOADDLL, &info);

            // ImageFileHandle is borrowed from DbgEng; the engine owns it.
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(UnloadModule)(PCWSTR ImageBaseName, ULONG64 BaseOffset) override
    {
        logDebug("[{}] Module unloaded", __func__);
        logDebug("  ImageBaseName: {}", ImageBaseName ? Utf16ToUtf8(ImageBaseName) : "<unknown>");
        logDebug("  BaseOffset: {:#x}", BaseOffset);

        auto work = [=]
        {
            UNLOAD_DLL_DEBUG_INFO info = {};
            info.lpBaseOfDll = (LPVOID)BaseOffset;
            setFakeDebugEvent(info);
            dispatchDebugEvent(UE_CH_UNLOADDLL, &info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(SystemError)(ULONG Error, ULONG Level) override
    {
        logDebug("[{}] System error", __func__);
        logDebug("  Error: {:#x}", Error);
        logDebug("  Level: {:#x}", Level);
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(SessionStatus)(ULONG Status) override
    {
        logDebug("[{}] Session status changed", __func__);
        logSingleFlag("Status", sessionFlags, Status);
        if (Status == DEBUG_SESSION_ACTIVE)
            mLastCurrentThread = DEBUG_ANY_ID;
        return S_OK; // ignored
    }

    STDMETHOD(ChangeDebuggeeState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("[{}] Debuggee state changed", __func__);
        logSingleFlag("Flags", cdsFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);
        return S_OK; // ignored
    }

    STDMETHOD(ChangeEngineState)(ULONG Flags, ULONG64 Argument) override
    {
        auto flagsToLog = Flags;
        if (Flags == DEBUG_CES_CURRENT_THREAD)
        {
            // DbgEng frequently re-announces engine thread index 0 while
            // loading modules even though the current thread did not change.
            // Keep real thread switches visible, but suppress those duplicate
            // notifications instead of disabling useful tracing globally.
            if (Argument == mLastCurrentThread)
                return S_OK;
            mLastCurrentThread = Argument;
        }

        logDebug("[{}] Engine state changed (tid: {}, apc: {}, debug: {})", __func__, GetCurrentThreadId(), mEventThreadId, gDebugThreadId);
        logBitFlag("Flags", cesFlags, flagsToLog);
        logDebug("  Argument: {:#x}", Argument);
        if (flagsToLog & DEBUG_CES_EXECUTION_STATUS)
        {
            if (Argument & DEBUG_STATUS_INSIDE_WAIT)
            {
                logDebug("  DEBUG_STATUS_INSIDE_WAIT");
            }
            if (Argument & DEBUG_STATUS_WAIT_TIMEOUT)
            {
                logDebug("  DEBUG_STATUS_WAIT_TIMEOUT");
            }
            logSingleFlag("Status", statusFlags, Argument);

            // Step completion is recognized after WaitForEvent returns with no
            // event-specific callback. A status transition to STEP_* only means
            // that execution has started; treating it as completion races other
            // target threads and resumes scripts too early.
        }
        return S_OK;
    }

    STDMETHOD(ChangeSymbolState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("[{}] Symbol state changed", __func__);
        logBitFlag("Flags", cssFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);

        return S_OK; // ignored
    }
};

static DebugEventCallbacks* gEventCallbacks = nullptr;

class DebugOutputCallbacks : public IDebugOutputCallbacksWide
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** object) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDebugOutputCallbacksWide))
        {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&mRefCount); }
    STDMETHOD_(ULONG, Release)() override
    {
        const auto count = InterlockedDecrement(&mRefCount);
        if (!count)
            delete this;
        return count;
    }
    STDMETHOD(Output)(ULONG mask, PCWSTR text) override
    {
        if (!(mask & DEBUG_OUTPUT_DEBUGGEE) || !text)
            return S_OK;

        const auto utf8 = Utf16ToUtf8(text);
        OUTPUT_DEBUG_STRING_INFO info = {};
        info.lpDebugStringData = const_cast<char*>(utf8.data());
        info.nDebugStringLength = (WORD)std::min<size_t>(utf8.size() + 1, USHRT_MAX);
        gSyntheticDebugString = utf8.data();
        gSyntheticDebugStringSize = utf8.size() + 1;

        // DbgEng reports debuggee output through this informational callback
        // without ending WaitForEvent. Deliver it synchronously so it is not
        // delayed until an unrelated target event. Avoid calling back into
        // DbgEng from this callback; retain the current process/thread IDs.
        const auto processId = gFakeDebugEvent.dwProcessId;
        const auto threadId = gFakeDebugEvent.dwThreadId;
        gFakeDebugEvent = {};
        gFakeDebugEvent.dwDebugEventCode = OUTPUT_DEBUG_STRING_EVENT;
        gFakeDebugEvent.dwProcessId = processId;
        gFakeDebugEvent.dwThreadId = threadId;
        gFakeDebugEvent.u.DebugString = info;
        invokeCustomHandler(UE_CH_DEBUGEVENT, &gFakeDebugEvent);
        invokeCustomHandler(UE_CH_OUTPUTDEBUGSTRING, &info);

        gSyntheticDebugString = nullptr;
        gSyntheticDebugStringSize = 0;
        return S_OK;
    }

private:
    ULONG mRefCount = 1;
};

static DebugOutputCallbacks* gOutputCallbacks = nullptr;

struct RegisterCache
{
    ULONG Count = 0;
    std::vector<ULONG> Indices;
    std::vector<DEBUG_VALUE> Values;
    std::vector<DEBUG_REGISTER_DESCRIPTION> Descriptions;
    std::vector<std::string> Names;
    std::vector<uint8_t> Changed;

    static inline std::map<const char*, TitanRegister> TitanRegNameMap = {
        { "eax", UE_EAX },
        { "ebx", UE_EBX },
        { "ecx", UE_ECX },
        { "edx", UE_EDX },
        { "edi", UE_EDI },
        { "esi", UE_ESI },
        { "ebp", UE_EBP },
        { "esp", UE_ESP },
        { "eip", UE_EIP },
        { "efl", UE_EFLAGS },
        { "dr0", UE_DR0 },
        { "dr1", UE_DR1 },
        { "dr2", UE_DR2 },
        { "dr3", UE_DR3 },
        { "dr6", UE_DR6 },
        { "dr7", UE_DR7 },
        { "rax", UE_RAX },
        { "rbx", UE_RBX },
        { "rcx", UE_RCX },
        { "rdx", UE_RDX },
        { "rdi", UE_RDI },
        { "rsi", UE_RSI },
        { "rbp", UE_RBP },
        { "rsp", UE_RSP },
        { "rip", UE_RIP },
        { "efl", UE_RFLAGS },
        { "r8", UE_R8 },
        { "r9", UE_R9 },
        { "r10", UE_R10 },
        { "r11", UE_R11 },
        { "r12", UE_R12 },
        { "r13", UE_R13 },
        { "r14", UE_R14 },
        { "r15", UE_R15 },
#ifdef _WIN64
        { "rip", UE_CIP },
        { "rsp", UE_CSP },
#else
        { "eip", UE_CIP },
        { "esp", UE_CSP },
#endif
        { "gs", UE_SEG_GS },
        { "fs", UE_SEG_FS },
        { "es", UE_SEG_ES },
        { "ds", UE_SEG_DS },
        { "cs", UE_SEG_CS },
        { "ss", UE_SEG_SS },
        { "st0", UE_x87_r0 }, // TODO: is this right?
        { "st1", UE_x87_r1 },
        { "st2", UE_x87_r2 },
        { "st3", UE_x87_r3 },
        { "st4", UE_x87_r4 },
        { "st5", UE_x87_r5 },
        { "st6", UE_x87_r6 },
        { "st7", UE_x87_r7 },
        { "fpsw", UE_X87_STATUSWORD },
        { "fpcw", UE_X87_CONTROLWORD },
        { "fptw", UE_X87_TAGWORD },
        { "mxcsr", UE_MXCSR },
        { "mm0", UE_MMX0 },
        { "mm1", UE_MMX1 },
        { "mm2", UE_MMX2 },
        { "mm3", UE_MMX3 },
        { "mm4", UE_MMX4 },
        { "mm5", UE_MMX5 },
        { "mm6", UE_MMX6 },
        { "mm7", UE_MMX7 },
        { "xmm0", UE_XMM0 },
        { "xmm1", UE_XMM1 },
        { "xmm2", UE_XMM2 },
        { "xmm3", UE_XMM3 },
        { "xmm4", UE_XMM4 },
        { "xmm5", UE_XMM5 },
        { "xmm6", UE_XMM6 },
        { "xmm7", UE_XMM7 },
        { "xmm8", UE_XMM8 },
        { "xmm9", UE_XMM9 },
        { "xmm10", UE_XMM10 },
        { "xmm11", UE_XMM11 },
        { "xmm12", UE_XMM12 },
        { "xmm13", UE_XMM13 },
        { "xmm14", UE_XMM14 },
        { "xmm15", UE_XMM15 },
        { "st0", UE_x87_ST0 },
        { "st1", UE_x87_ST1 },
        { "st2", UE_x87_ST2 },
        { "st3", UE_x87_ST3 },
        { "st4", UE_x87_ST4 },
        { "st5", UE_x87_ST5 },
        { "st6", UE_x87_ST6 },
        { "st7", UE_x87_ST7 },
        { "ymm0", UE_YMM0 },
        { "ymm1", UE_YMM1 },
        { "ymm2", UE_YMM2 },
        { "ymm3", UE_YMM3 },
        { "ymm4", UE_YMM4 },
        { "ymm5", UE_YMM5 },
        { "ymm6", UE_YMM6 },
        { "ymm7", UE_YMM7 },
        { "ymm8", UE_YMM8 },
        { "ymm9", UE_YMM9 },
        { "ymm10", UE_YMM10 },
        { "ymm11", UE_YMM11 },
        { "ymm12", UE_YMM12 },
        { "ymm13", UE_YMM13 },
        { "ymm14", UE_YMM14 },
        { "ymm15", UE_YMM15 },
    };

    std::map<TitanRegister, ULONG> TitanRegIndexMap;

    bool Read()
    {
        if (Count == 0)
        {
            auto hr = gDebugRegisters->GetNumberRegisters(&Count);
            if (FAILED(hr))
            {
                logError("Failed to get number of registers: {:#x}", (uint32_t)hr);
                return false;
            }

            Indices.resize(Count);
            Values.resize(Count);
            Descriptions.resize(Count);
            Names.resize(Count);
            Changed.resize(Count);
            for (ULONG i = 0; i < Count; i++)
            {
                char name[256] = {};
                DEBUG_REGISTER_DESCRIPTION description = {};
                hr = gDebugRegisters->GetDescription(i, name, sizeof(name), nullptr, &description);
                if (FAILED(hr))
                {
                    logError("Failed to get description for register {}: {:#x}", i, (uint32_t)hr);
                    return false;
                }
                Indices[i] = i;
                Descriptions[i] = description;
                Names[i] = name;
                logDebug("register[{}]: {}, length: {}, type: {}, flags: {}", i, name, description.SubregLength, description.Type, description.Flags);
                for (const auto& [titanName, titanReg] : TitanRegNameMap)
                {
                    if (_stricmp(name, titanName) == 0)
                    {
                        logDebug("  TitanRegister: {}", (int32_t)titanReg);
                        TitanRegIndexMap[titanReg] = i;
                        // NOTE: no break on purpose due to aliases
                    }
                }
                if (_stricmp(name, "efl") == 0)
                {
                    TitanRegIndexMap[UE_EFLAGS] = i;
                    TitanRegIndexMap[UE_RFLAGS] = i;
                }
#ifdef _WIN64
                else if (_stricmp(name, "rip") == 0)
                    TitanRegIndexMap[UE_CIP] = i;
                else if (_stricmp(name, "rsp") == 0)
                    TitanRegIndexMap[UE_CSP] = i;
#else
                if (_stricmp(name, "eip") == 0)
                    TitanRegIndexMap[UE_CIP] = i;
                else if (_stricmp(name, "esp") == 0)
                    TitanRegIndexMap[UE_CSP] = i;
#endif
            }
        }

        for (ULONG i = 0; i < Count; i++)
        {
            if (Changed[i])
            {
                logDebug("Register {} ({}) marked as changed", i, Names[i]);
            }
        }

        // https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dbgeng/nf-dbgeng-idebugregisters2-getvalues2#remarks
        //
        auto hr = gDebugRegisters->GetValues(Count, Indices.data(), 0, Values.data());
        if (hr == E_UNEXPECTED)
        {
            logError("Failed to get register values");
            return false;
        }

        for (ULONG i = 0; i < Count; i++)
        {
            if (Values[i].Type == DEBUG_VALUE_INVALID)
            {
                // logError("register[{}]: {} has DEBUG_VALUE_INVALID", i, Names[i]);
            }
        }

        return true;
    }

    bool Flush()
    {
        std::vector<ULONG> changedIndices;
        std::vector<DEBUG_VALUE> changedValues;
        for (ULONG i = 0; i < Count; i++)
        {
            if (Changed[i])
            {
                logDebug("Writing register {} ({})", i, Names[i]);
                changedIndices.push_back(i);
                changedValues.push_back(Values[i]);
            }
        }
        if (changedIndices.empty())
            return true;

        auto hr = gDebugRegisters->SetValues((ULONG)changedIndices.size(), changedIndices.data(), 0, changedValues.data());
        if (FAILED(hr))
        {
            logError("Failed to write register values: {:#x}", (uint32_t)hr);
            return false;
        }
        std::fill(Changed.begin(), Changed.end(), 0);
        return true;
    }

    bool SetValue(TitanRegister reg, ULONG_PTR newValue)
    {
        auto itr = TitanRegIndexMap.find(reg);
        if (itr == TitanRegIndexMap.end())
        {
            logError("Unknown TitanRegister: {}", (int32_t)reg);
            return false;
        }

        const auto index = itr->second;
        auto& value = Values[index];
        switch (value.Type)
        {
        case DEBUG_VALUE_INT8:
            value.I8 = (UCHAR)newValue;
            break;
        case DEBUG_VALUE_INT16:
            value.I16 = (USHORT)newValue;
            break;
        case DEBUG_VALUE_INT32:
            value.I32 = (ULONG)newValue;
            break;
        case DEBUG_VALUE_INT64:
            value.I64 = (ULONG64)newValue;
            break;
        default:
            logError("Register {} ({}) cannot be assigned as an integer", index, Names[index]);
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        Changed[index] = true;
        return true;
    }

    const DEBUG_VALUE* FindValue(const char* name) const
    {
        for (size_t i = 0; i < Names.size(); ++i)
        {
            if (_stricmp(Names[i].c_str(), name) == 0 && Values[i].Type != DEBUG_VALUE_INVALID)
                return &Values[i];
        }
        return nullptr;
    }

    ULONG_PTR GetValue(TitanRegister reg)
    {
        auto itr = TitanRegIndexMap.find(reg);
        if (itr == TitanRegIndexMap.end())
        {
            logError("Unknown TitanRegister: {}", (int32_t)reg);
            return 0;
        }
        auto index = itr->second;
        const auto& value = Values[index];
        switch (value.Type)
        {
        case DEBUG_VALUE_INVALID:
            logError("register[{}]: {} has DEBUG_VALUE_INVALID", index, Names[index]);
            return 0;
        case DEBUG_VALUE_INT8:
            return value.I8;
        case DEBUG_VALUE_INT16:
            return value.I16;
        case DEBUG_VALUE_INT32:
            return value.I32;
        case DEBUG_VALUE_INT64:
            return value.I64;
        case DEBUG_VALUE_FLOAT32:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT32", index, Names[index]);
            return 0;
        case DEBUG_VALUE_FLOAT64:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT64", index, Names[index]);
            return 0;
        case DEBUG_VALUE_FLOAT80:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT80", index, Names[index]);
            return 0;
        case DEBUG_VALUE_FLOAT82:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT82", index, Names[index]);
            return 0;
        case DEBUG_VALUE_FLOAT128:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT128", index, Names[index]);
            return 0;
        case DEBUG_VALUE_VECTOR64:
            logError("register[{}]: {} has DEBUG_VALUE_VECTOR64", index, Names[index]);
            return 0;
        case DEBUG_VALUE_VECTOR128:
            logError("register[{}]: {} has DEBUG_VALUE_VECTOR128", index, Names[index]);
            return 0;
        default:
            break;
        }
        return 0;
    }
};

static RegisterCache gRegisterCache;

struct NativeContextBuffer
{
    std::vector<BYTE> storage;
    PCONTEXT context = nullptr;

    bool Load(HANDLE thread, DWORD64 requestedFeatures)
    {
        DWORD length = 0;
        InitializeContext(nullptr, CONTEXT_ALL | CONTEXT_XSTATE, &context, &length);
        if (!length)
            return false;
        storage.resize(length);
        if (!InitializeContext(storage.data(), CONTEXT_ALL | CONTEXT_XSTATE, &context, &length))
            return false;
        const auto enabled = GetEnabledXStateFeatures();
        if (!SetXStateFeaturesMask(context, requestedFeatures & enabled))
            return false;
        context->ContextFlags = CONTEXT_ALL | CONTEXT_XSTATE;
        return !!GetThreadContext(thread, context);
    }

    void* Feature(DWORD id, DWORD minimumSize = 0) const
    {
        DWORD size = 0;
        auto result = LocateXStateFeature(context, id, &size);
        return result && size >= minimumSize ? result : nullptr;
    }
};

static void readExtendedContext(const NativeContextBuffer& native, TITAN_ENGINE_CONTEXT_t* titan)
{
#ifdef _WIN64
    const auto* save = &native.context->FltSave;
#else
    const auto* save = reinterpret_cast<const XSAVE_FORMAT*>(native.context->ExtendedRegisters);
#endif
    titan->x87fpu.ControlWord = save->ControlWord;
    titan->x87fpu.StatusWord = save->StatusWord;
    titan->x87fpu.TagWord = save->TagWord;
    titan->x87fpu.ErrorOffset = save->ErrorOffset;
    titan->x87fpu.ErrorSelector = save->ErrorSelector;
    titan->x87fpu.DataOffset = save->DataOffset;
    titan->x87fpu.DataSelector = save->DataSelector;
    titan->MxCsr = save->MxCsr;
    for (size_t i = 0; i < 8; ++i)
        memcpy(titan->RegisterArea + i * 10, &save->FloatRegisters[i], 10);
    for (size_t i = 0; i < std::size(titan->XmmRegisters); ++i)
    {
        memcpy(&titan->XmmRegisters[i], &save->XmmRegisters[i], sizeof(XmmRegister_t));
        titan->YmmRegisters[i].Low = titan->XmmRegisters[i];
    }

    if (auto avx = static_cast<const XmmRegister_t*>(native.Feature(XSTATE_AVX, std::size(titan->YmmRegisters) * sizeof(XmmRegister_t))))
    {
        for (size_t i = 0; i < std::size(titan->YmmRegisters); ++i)
            titan->YmmRegisters[i].High = avx[i];
    }
}

static void writeExtendedContext(NativeContextBuffer& native, const TITAN_ENGINE_CONTEXT_t* titan)
{
#ifdef _WIN64
    auto* save = &native.context->FltSave;
#else
    auto* save = reinterpret_cast<XSAVE_FORMAT*>(native.context->ExtendedRegisters);
#endif
    save->ControlWord = titan->x87fpu.ControlWord;
    save->StatusWord = titan->x87fpu.StatusWord;
    save->TagWord = (BYTE)titan->x87fpu.TagWord;
    save->ErrorOffset = titan->x87fpu.ErrorOffset;
    save->ErrorSelector = titan->x87fpu.ErrorSelector;
    save->DataOffset = titan->x87fpu.DataOffset;
    save->DataSelector = titan->x87fpu.DataSelector;
    save->MxCsr = titan->MxCsr;
#ifdef _WIN64
    native.context->MxCsr = titan->MxCsr;
#endif
    for (size_t i = 0; i < 8; ++i)
        memcpy(&save->FloatRegisters[i], titan->RegisterArea + i * 10, 10);
    for (size_t i = 0; i < std::size(titan->XmmRegisters); ++i)
        memcpy(&save->XmmRegisters[i], &titan->XmmRegisters[i], sizeof(XmmRegister_t));

    if (auto avx = static_cast<XmmRegister_t*>(native.Feature(XSTATE_AVX, std::size(titan->YmmRegisters) * sizeof(XmmRegister_t))))
    {
        for (size_t i = 0; i < std::size(titan->YmmRegisters); ++i)
            avx[i] = titan->YmmRegisters[i].High;
    }
}

static void setCurrentInstructionPointer(ULONG_PTR address)
{
    gRegisterCache.SetValue(UE_CIP, address);
}

struct DebugThreadScope
{
    explicit DebugThreadScope(HANDLE threadHandle)
    {
        auto found = gDebugIdMap.threadHandleToIndex.find(threadHandle);
        if (found == gDebugIdMap.threadHandleToIndex.end())
        {
            logError("Unknown thread handle: {:#x}", (uint64_t)(ULONG_PTR)threadHandle);
            SetLastError(ERROR_INVALID_HANDLE);
            return;
        }

        auto hr = gDebugSystemObjects->GetCurrentThreadId(&mPreviousIndex);
        if (FAILED(hr))
        {
            logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
            return;
        }

        const auto requestedIndex = found->second;
        if (requestedIndex != mPreviousIndex)
        {
            if (!gRegisterCache.Flush())
                return;
            hr = gDebugSystemObjects->SetCurrentThreadId(requestedIndex);
            if (FAILED(hr))
            {
                logError("Failed to select thread index {}: {:#x}", requestedIndex, (uint32_t)hr);
                return;
            }
            mSwitched = true;
            if (!gRegisterCache.Read())
                return;
        }
        mValid = true;
    }

    ~DebugThreadScope()
    {
        if (!mSwitched)
            return;
        gRegisterCache.Flush();
        const auto hr = gDebugSystemObjects->SetCurrentThreadId(mPreviousIndex);
        if (FAILED(hr))
            logError("Failed to restore thread index {}: {:#x}", mPreviousIndex, (uint32_t)hr);
        else
            gRegisterCache.Read();
    }

    explicit operator bool() const { return mValid; }

private:
    ULONG mPreviousIndex = 0;
    bool mSwitched = false;
    bool mValid = false;
};

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static HANDLE gDbgEngEvent = nullptr;
static HANDLE gDbgEngThread = nullptr;

static bool InitializeDbgEngImpl()
{
    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        logError("Failed to initialize COM in dbgeng thread: {:#x}", (uint32_t)hr);
        return false;
    }
    gComInitialized = true;

    // Create primary debug client interface
    hr = DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(&gDebugClient));
    if (FAILED(hr))
    {
        logError("Failed to create IDebugClient5: {:#x}", (uint32_t)hr);
        return false;
    }

    // Query for other interfaces
    hr = debugClientInterface(&gDebugControl);
    if (FAILED(hr))
    {
        logError("Failed to get IDebugControl: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = debugClientInterface(&gDebugDataSpaces);
    if (FAILED(hr))
    {
        logError("Failed to get IDebugDataSpaces: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = debugClientInterface(&gDebugRegisters);
    if (FAILED(hr))
    {
        logError("Failed to get IDebugRegisters: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = debugClientInterface(&gDebugSymbols);
    if (FAILED(hr))
    {
        logError("Failed to get IDebugSymbols: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = debugClientInterface(&gDebugSystemObjects);
    if (FAILED(hr))
    {
        logError("Failed to get IDebugSystemObjects: {:#x}", (uint32_t)hr);
        return false;
    }

    // Set up event callbacks
    gEventCallbacks = new DebugEventCallbacks();
    hr = gDebugClient->SetEventCallbacksWide(gEventCallbacks);
    if (FAILED(hr))
    {
        logError("Failed to set event callbacks: {:#x}", (uint32_t)hr);
        return false;
    }

    gOutputCallbacks = new DebugOutputCallbacks();
    hr = gDebugClient->SetOutputCallbacksWide(gOutputCallbacks);
    if (FAILED(hr))
    {
        logError("Failed to set output callbacks: {:#x}", (uint32_t)hr);
        return false;
    }

    // Set engine options for initial break
    hr = gDebugControl->SetEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK | DEBUG_ENGOPT_DISABLE_MODULE_SYMBOL_LOAD);
    if (FAILED(hr))
    {
        logError("Failed to set engine options: {:#x}", (uint32_t)hr);
        return false;
    }

    logDebug("Initialization complete!");

    return true;
}

static void ShutdownDbgEngImpl()
{
    if (gDebugClient)
    {
        gDebugClient->SetOutputCallbacksWide(nullptr);
        gDebugClient->SetEventCallbacksWide(nullptr);
    }
    if (gOutputCallbacks)
    {
        gOutputCallbacks->Release();
        gOutputCallbacks = nullptr;
    }
    if (gEventCallbacks)
    {
        gEventCallbacks->Release();
        gEventCallbacks = nullptr;
    }

    const auto release = [](auto*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    };
    release(gDebugSystemObjects);
    release(gDebugSymbols);
    release(gDebugRegisters);
    release(gDebugDataSpaces);
    release(gDebugControl);
    release(gDebugClient);
    if (gComInitialized)
    {
        CoUninitialize();
        gComInitialized = false;
    }
    gDbgEngInitialized = false;
}

static bool InitializeDbgEng()
{
    if (gDbgEngInitialized)
        return true;

    _plugin_logprintf = []
    {
#ifdef _WIN64
        auto hModule = GetModuleHandleW(L"x64dbg.dll");
#else
        auto hModule = GetModuleHandleW(L"x32dbg.dll");
#endif // _WIN64
        if (hModule)
        {
            auto result = (decltype(&printf))GetProcAddress(hModule, "_plugin_logprintf");
            if (result)
                return result;
        }
        puts("Could not find _plugin_logprintf!");
        return printf;
    }();

    wchar_t moduleDir[MAX_PATH] = {};
    GetModuleFileNameW((HMODULE)&__ImageBase, moduleDir, std::size(moduleDir));

    auto lastSlash = wcsrchr(moduleDir, L'\\');
    if (lastSlash)
        *(lastSlash + 1) = L'\0';

    auto loadDll = [&moduleDir](const wchar_t* dllName) -> HMODULE
    {
        HMODULE hModule = GetModuleHandleW(dllName);
        if (hModule)
        {
            logDebug("{} already loaded (might cause conflicts)", Utf16ToUtf8(dllName));
            return hModule;
        }

        auto dllPath = std::wstring(moduleDir) + dllName;
        hModule = LoadLibraryW(dllPath.c_str());
        if (!hModule)
        {
            logError("Failed to load {}: {:#x}", Utf16ToUtf8(dllName), (uint32_t)GetLastError());
        }
        return hModule;
    };
    if (!loadDll(L"dbgmodel.dll"))
        return false;
    if (!loadDll(L"dbgeng.dll"))
        return false;

    // NOTE: We initialize dbgeng on a separate thread because this thread is used to deliver
    // events to and we do not want this in the UI thread.
    auto dbgEngThreadProc = [](void* initEvent) -> DWORD
    {
        logDebug("dbgeng thread started");
        gDbgEngInitialized = InitializeDbgEngImpl();
        SetEvent((HANDLE)initEvent);

        // Wait in an alertable state so that debug events can be delivered
        while (true)
        {
            auto result = WaitForSingleObjectEx(gDbgEngEvent, 1, TRUE);
            if (result == WAIT_OBJECT_0)
            {
                logDebug("dbgeng thread exiting");
                break;
            }
            else if (result == WAIT_FAILED)
            {
                logError("WaitForSingleObjectEx failed in dbgeng thread: {:#x}", (uint32_t)GetLastError());
                break;
            }
        }

        ShutdownDbgEngImpl();

        // If the gDbgEngEvent is signaled, we should exit the thread
        return 0;
    };

    auto initEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gDbgEngEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gProcessCreatedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gDbgEngThread = CreateThread(nullptr, 0, dbgEngThreadProc, initEvent, 0, nullptr);
    WaitForSingleObject(initEvent, INFINITE);
    CloseHandle(initEvent);

    if (!gDbgEngInitialized)
    {
        SetEvent(gDbgEngEvent);
        WaitForSingleObject(gDbgEngThread, INFINITE);
        CloseHandle(gDbgEngThread);
        CloseHandle(gDbgEngEvent);
        CloseHandle(gProcessCreatedEvent);
        gDbgEngEvent = nullptr;
        gProcessCreatedEvent = nullptr;
        gDbgEngThread = nullptr;
        logError("Failed to initialize dbgeng");
        return false;
    }

    return true;
}

// TitanEngine.Dumper.functions:
__declspec(dllexport) ULONG_PTR ConvertVAtoFileOffsetEx(ULONG_PTR FileMapVA, DWORD FileSize, ULONG_PTR ImageBase, ULONG_PTR AddressToConvert, bool AddressIsRVA, bool ReturnType)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

__declspec(dllexport) ULONG_PTR ConvertFileOffsetToVA(ULONG_PTR FileMapVA, ULONG_PTR AddressToConvert, bool ReturnType)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

__declspec(dllexport) bool MemoryReadUnsafe(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    const auto readAddress = (ULONG_PTR)lpBaseAddress;
    const auto syntheticAddress = (ULONG_PTR)gSyntheticDebugString;
    if (gSyntheticDebugString && readAddress >= syntheticAddress &&
        readAddress + nSize <= syntheticAddress + gSyntheticDebugStringSize)
    {
        memcpy(lpBuffer, lpBaseAddress, nSize);
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = nSize;
        return true;
    }

    // NOTE: This happens because x64dbg refreshes the memory map before cbCreateProcess in the background
    if (!hProcess)
        return false;

    // NOTE: This happens because the GUI reads in the background
    if ((ULONG_PTR)lpBaseAddress < 0x10000)
        return false;

    PauseLock pl;
    if (!pl)
    {
        return false;
    }

    auto itr = gDebugIdMap.processHandleToIndex.find(hProcess);
    if (itr == gDebugIdMap.processHandleToIndex.end())
    {
        SIZE_T bytesRead = 0;
        const auto result = !!ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, &bytesRead);
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = bytesRead;
        return result;
    }

    auto processIndex = itr->second;
    ULONG currentIndex = 0;
    auto hr = gDebugSystemObjects->GetCurrentProcessId(&currentIndex);
    if (FAILED(hr))
    {
        logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
        return 0;
    }
    if (currentIndex != processIndex)
    {
        logError("MemoryReadUnsafe does not support a non-current process yet");
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }

    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        if (nSize > ULONG_MAX)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        ULONG bytesRead = 0;
        hr = gDebugDataSpaces->ReadVirtual((ULONG64)(ULONG_PTR)lpBaseAddress, lpBuffer, (ULONG)nSize, &bytesRead);
        if (lpNumberOfBytesRead)
            *lpNumberOfBytesRead = bytesRead;
        if (FAILED(hr) || bytesRead != nSize)
        {
            SetLastError(bytesRead ? ERROR_PARTIAL_COPY : ERROR_READ_FAULT);
            return false;
        }
        return true;
    }

    // Read the live process directly so DbgEng cannot substitute original
    // bytes for software breakpoints. Watched pages are exposed only for the
    // duration of this debugger read and are re-guarded before returning.
    std::vector<ULONG_PTR> guardedPages;
    if (nSize && (ULONG_PTR)lpBaseAddress + nSize > (ULONG_PTR)lpBaseAddress)
    {
        const auto firstPage = pageAddress((ULONG_PTR)lpBaseAddress);
        const auto lastPage = pageAddress((ULONG_PTR)lpBaseAddress + nSize - 1);
        for (auto page = firstPage;; page += targetPageSize())
        {
            if (gMemoryBreakpointPages.contains(page) && protectMemoryBreakpointPage(page, false))
                guardedPages.push_back(page);
            if (page == lastPage)
                break;
        }
    }

    SIZE_T bytesRead = 0;
    const auto result = !!ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, &bytesRead);
    for (const auto page : guardedPages)
        protectMemoryBreakpointPage(page, true);
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = bytesRead;
    return result && bytesRead == nSize;
}

__declspec(dllexport) bool MemoryReadSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    if (!MemoryReadUnsafe(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead))
        return false;

    const auto readStart = (ULONG_PTR)lpBaseAddress;
    const auto readEnd = readStart + nSize;
    PauseLock pl;
    if (!pl)
        return false;
    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.kind != BreakpointKind::Software || !info.nativePatch || !info.patchSize)
            continue;
        const auto breakpointStart = (ULONG_PTR)info.offset;
        const auto breakpointEnd = breakpointStart + info.patchSize;
        const auto overlapStart = (std::max)(readStart, breakpointStart);
        const auto overlapEnd = (std::min)(readEnd, breakpointEnd);
        if (overlapStart < overlapEnd)
        {
            memcpy((BYTE*)lpBuffer + (overlapStart - readStart),
                   info.originalBytes + (overlapStart - breakpointStart),
                   overlapEnd - overlapStart);
        }
    }
    return true;
}

__declspec(dllexport) bool MemoryWriteSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = 0;
    if (!hProcess || !lpBaseAddress || !lpBuffer || !nSize || nSize > ULONG_MAX)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        SetLastError(ERROR_BUSY);
        return false;
    }
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }

    ULONG written = 0;
    auto hr = gDebugDataSpaces->WriteVirtual((ULONG64)(ULONG_PTR)lpBaseAddress, const_cast<PVOID>(lpBuffer), (ULONG)nSize, &written);
    if (lpNumberOfBytesWritten)
        *lpNumberOfBytesWritten = written;
    if (FAILED(hr))
    {
        logError("WriteVirtual failed: {:#x}", (uint32_t)hr);
        SetLastError(ERROR_WRITE_FAULT);
        return false;
    }
    return written == nSize;
}

__declspec(dllexport) SIZE_T MemoryQuerySafe(HANDLE hProcess, LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
{
    if (!hProcess || !lpBuffer || dwLength < sizeof(*lpBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        PauseLock pl;
        if (!pl || !gDebugIdMap.processHandleToIndex.contains(hProcess))
        {
            SetLastError(pl ? ERROR_INVALID_HANDLE : ERROR_BUSY);
            return 0;
        }
        MEMORY_BASIC_INFORMATION64 info = {};
        const auto hr = gDebugDataSpaces->QueryVirtual((ULONG64)(ULONG_PTR)lpAddress, &info);
        if (FAILED(hr))
        {
            SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
        *lpBuffer = {};
        lpBuffer->BaseAddress = (PVOID)(ULONG_PTR)info.BaseAddress;
        lpBuffer->AllocationBase = (PVOID)(ULONG_PTR)info.AllocationBase;
        lpBuffer->AllocationProtect = info.AllocationProtect;
        lpBuffer->RegionSize = (SIZE_T)info.RegionSize;
        lpBuffer->State = info.State;
        lpBuffer->Protect = info.Protect;
        lpBuffer->Type = info.Type;
        return sizeof(*lpBuffer);
    }
    return VirtualQueryEx(hProcess, lpAddress, lpBuffer, dwLength);
}

__declspec(dllexport) LPVOID MemoryAllocSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    return VirtualAllocEx(hProcess, lpAddress, dwSize, flAllocationType, flProtect);
}

__declspec(dllexport) bool MemoryFreeSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    return !!VirtualFreeEx(hProcess, lpAddress, dwSize, dwFreeType);
}

__declspec(dllexport) bool MemoryProtectSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    return !!VirtualProtectEx(hProcess, lpAddress, dwSize, flNewProtect, lpflOldProtect);
}

// TitanEngine.Hider.functions:
__declspec(dllexport) ULONG_PTR GetPEBLocation(HANDLE hProcess)
{
    PauseLock pl;
    if (!pl)
    {
        return 0;
    }
    auto itr = gProcessPebCache.find(hProcess);
    if (itr == gProcessPebCache.end())
    {
        logError("Unknown process handle in GetPEBLocation: {:#x}", (uint64_t)(ULONG_PTR)hProcess);
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return itr->second;
}

__declspec(dllexport) ULONG_PTR GetTEBLocation(HANDLE hThread)
{
    PauseLock pl;
    if (!pl)
    {
        return 0;
    }
    auto itr = gProcessTebCache.find(hThread);
    if (itr == gProcessTebCache.end())
    {
        logError("Unknown thread handle in GetTEBLocation: {:#x}", (uint64_t)(ULONG_PTR)hThread);
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return itr->second;
}

// TitanEngine.Debugger.functions:
__declspec(dllexport) PROCESS_INFORMATION* InitDebugW(const wchar_t* szFileName, const wchar_t* szCommandLine, const wchar_t* szCurrentFolder)
{
    if (!szFileName || !*szFileName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    std::wstring commandLine = L"\"";
    commandLine += szFileName;
    commandLine += L"\"";
    if (szCommandLine && *szCommandLine)
    {
        commandLine += L" ";
        commandLine += szCommandLine;
    }

    gProcessInfo = {};
    ++gSessionGeneration;
    ResetEvent(gProcessCreatedEvent);
    gDebugThreadId = GetCurrentThreadId();
    gExpectSystemBreakpoint = true;
    gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
    gNextContinueStatus = DBG_CONTINUE;

    DEBUG_CREATE_PROCESS_OPTIONS options = {};
    options.CreateFlags = DEBUG_ONLY_THIS_PROCESS;
    if (gEngineVariables[UE_ENGINE_NO_CONSOLE_WINDOW])
        options.CreateFlags |= CREATE_NO_WINDOW;
    else
        options.CreateFlags |= CREATE_NEW_CONSOLE;
    auto hr = gDebugClient->CreateProcess2Wide(0, commandLine.data(), &options, sizeof(options), szCurrentFolder, nullptr);
    if (FAILED(hr))
    {
        logError("CreateProcess2Wide failed: {:#x}", (uint32_t)hr);
        SetLastError(HRESULT_CODE(hr));
        return nullptr;
    }

    if (WaitForSingleObject(gProcessCreatedEvent, 0) != WAIT_OBJECT_0)
    {
        hr = gDebugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr) || WaitForSingleObject(gProcessCreatedEvent, 0) != WAIT_OBJECT_0)
        {
            logError("Failed waiting for DbgEng create-process event: {:#x}", (uint32_t)hr);
            gDebugClient->EndSession(DEBUG_END_ACTIVE_TERMINATE);
            gProcessInfo = {};
            SetLastError(ERROR_GEN_FAILURE);
            return nullptr;
        }
    }

    HANDLE initProcess = nullptr;
    HANDLE initThread = nullptr;
    const auto duplicate = [&](HANDLE source, HANDLE* target)
    {
        return source && DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    };
    if (!duplicate(gProcessInfo.hProcess, &initProcess) || !duplicate(gProcessInfo.hThread, &initThread))
    {
        const auto error = GetLastError();
        if (initProcess)
            CloseHandle(initProcess);
        if (initThread)
            CloseHandle(initThread);
        gDebugClient->EndSession(DEBUG_END_ACTIVE_TERMINATE);
        gProcessInfo = {};
        SetLastError(error);
        return nullptr;
    }

    registerTitanHandle(initProcess, TitanHandleType::Process, DEBUG_ANY_ID, gProcessInfo.dwProcessId, true);
    registerTitanHandle(initThread, TitanHandleType::Thread, DEBUG_ANY_ID, gProcessInfo.dwThreadId, true);
    gProcessInfo.hProcess = initProcess;
    gProcessInfo.hThread = initThread;
    gSessionKind = UE_SESSION_LIVE;
    return &gProcessInfo;
}

__declspec(dllexport) PROCESS_INFORMATION* InitReplayW(const wchar_t* szArtifactPath, TitanSessionKind ExpectedKind)
{
    if (!szArtifactPath || !*szArtifactPath ||
        (ExpectedKind != UE_SESSION_MINIDUMP && ExpectedKind != UE_SESSION_TTD))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (gSessionKind != UE_SESSION_NONE || gIsDebugging)
    {
        SetLastError(ERROR_BUSY);
        return nullptr;
    }

    gProcessInfo = {};
    gDebugIdMap = {};
    gProcessPebCache = {};
    gProcessTebCache = {};
    closeCallerOwnedTitanHandles();
    ++gSessionGeneration;
    gDebugThreadId = GetCurrentThreadId();
    gExpectSystemBreakpoint = false;
    gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
    gNextContinueStatus = DBG_CONTINUE;
    {
        std::lock_guard lock(gMutexCallbackQueue);
        gCallbackQueue = {};
    }

    auto fail = [](HRESULT hr, DWORD fallbackError) -> PROCESS_INFORMATION*
    {
        if (gDebugClient)
            gDebugClient->EndSession(DEBUG_END_PASSIVE);
        {
            std::lock_guard lock(gMutexCallbackQueue);
            gCallbackQueue = {};
        }
        gProcessInfo = {};
        gDebugIdMap = {};
        gProcessPebCache = {};
        gProcessTebCache = {};
        closeCallerOwnedTitanHandles();
        gSessionKind = UE_SESSION_NONE;
        const auto error = FAILED(hr) && HRESULT_FACILITY(hr) == FACILITY_WIN32 && HRESULT_CODE(hr)
                               ? HRESULT_CODE(hr)
                               : fallbackError;
        SetLastError(error);
        return nullptr;
    };

    auto hr = gDebugClient->OpenDumpFileWide(szArtifactPath, 0);
    if (FAILED(hr))
    {
        logError("OpenDumpFileWide failed: {:#x}", (uint32_t)hr);
        return fail(hr, ERROR_BAD_FORMAT);
    }
    hr = gDebugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
    if (FAILED(hr))
    {
        logError("Initial replay WaitForEvent failed: {:#x}", (uint32_t)hr);
        return fail(hr, ERROR_BAD_FORMAT);
    }

    ULONG debugClass = 0;
    ULONG qualifier = 0;
    hr = gDebugControl->GetDebuggeeType(&debugClass, &qualifier);
    if (FAILED(hr) || debugClass != DEBUG_CLASS_USER_WINDOWS)
    {
        logError("Unsupported replay debuggee type: class {}, qualifier {}, hr {:#x}", debugClass, qualifier, (uint32_t)hr);
        return fail(hr, ERROR_NOT_SUPPORTED);
    }
    const auto detectedKind = qualifier == DEBUG_DUMP_TRACE_LOG ? UE_SESSION_TTD : UE_SESSION_MINIDUMP;
    if (detectedKind != ExpectedKind)
    {
        logError("Replay kind mismatch: expected {}, detected {} (qualifier {})", (uint32_t)ExpectedKind, (uint32_t)detectedKind, qualifier);
        return fail(E_INVALIDARG, ERROR_BAD_FORMAT);
    }
    if (detectedKind == UE_SESSION_TTD)
    {
        logError("TTD trace opening is recognized but navigation is not implemented yet");
        return fail(E_NOTIMPL, ERROR_NOT_SUPPORTED);
    }

    ULONG processCount = 0;
    hr = gDebugSystemObjects->GetNumberProcesses(&processCount);
    if (FAILED(hr) || processCount != 1)
    {
        logError("Replay requires exactly one process, found {}, hr {:#x}", processCount, (uint32_t)hr);
        return fail(hr, ERROR_NOT_SUPPORTED);
    }

    ULONG processIndex = DEBUG_ANY_ID;
    ULONG threadIndex = DEBUG_ANY_ID;
    ULONG processSystemId = 0;
    ULONG threadSystemId = 0;
    if (FAILED(hr = gDebugSystemObjects->GetCurrentProcessId(&processIndex)) ||
        FAILED(hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex)) ||
        FAILED(hr = gDebugSystemObjects->GetCurrentProcessSystemId(&processSystemId)) ||
        FAILED(hr = gDebugSystemObjects->GetCurrentThreadSystemId(&threadSystemId)))
    {
        logError("Failed to query replay process/thread identity: {:#x}", (uint32_t)hr);
        return fail(hr, ERROR_BAD_FORMAT);
    }

    ULONG threadCount = 0;
    hr = gDebugSystemObjects->GetNumberThreads(&threadCount);
    if (FAILED(hr) || !threadCount)
    {
        logError("Replay has no queryable threads: {:#x}", (uint32_t)hr);
        return fail(hr, ERROR_BAD_FORMAT);
    }
    std::vector<ULONG> threadEngineIds(threadCount);
    std::vector<ULONG> threadSystemIds(threadCount);
    hr = gDebugSystemObjects->GetThreadIdsByIndex(0, threadCount, threadEngineIds.data(), threadSystemIds.data());
    if (FAILED(hr))
    {
        logError("GetThreadIdsByIndex failed: {:#x}", (uint32_t)hr);
        return fail(hr, ERROR_BAD_FORMAT);
    }

    const auto eventProcess = createSyntheticTitanHandle(TitanHandleType::Process, processIndex, processSystemId, false);
    const auto eventThread = createSyntheticTitanHandle(TitanHandleType::Thread, threadIndex, threadSystemId, false);
    gDebugIdMap.processIndexToHandle[processIndex] = eventProcess;
    gDebugIdMap.processHandleToIndex[eventProcess] = processIndex;
    gDebugIdMap.threadIndexToHandle[threadIndex] = eventThread;
    gDebugIdMap.threadHandleToIndex[eventThread] = threadIndex;

    ULONG64 peb = 0;
    ULONG64 teb = 0;
    gDebugSystemObjects->GetCurrentProcessPeb(&peb);
    gDebugSystemObjects->GetCurrentThreadTeb(&teb);
    gProcessPebCache[eventProcess] = peb;
    gProcessTebCache[eventThread] = teb;

    ULONG64 imageBase = 0;
    ULONG64 instruction = 0;
    gDebugSymbols->GetModuleByIndex(0, &imageBase);
    gDebugRegisters->GetInstructionOffset(&instruction);

    EXCEPTION_DEBUG_INFO initialException = {};
    initialException.ExceptionRecord.ExceptionCode = EXCEPTION_BREAKPOINT;
    initialException.ExceptionRecord.ExceptionAddress = (PVOID)(ULONG_PTR)instruction;
    ULONG eventType = 0;
    ULONG eventProcessIndex = 0;
    ULONG eventThreadIndex = 0;
    ULONG extraUsed = 0;
    EXCEPTION_RECORD64 exception64 = {};
    if (SUCCEEDED(gDebugControl->GetLastEventInformationWide(&eventType, &eventProcessIndex, &eventThreadIndex,
                                                             &exception64, sizeof(exception64), &extraUsed,
                                                             nullptr, 0, nullptr)) &&
        eventType == DEBUG_EVENT_EXCEPTION && extraUsed >= sizeof(exception64))
    {
        initialException.ExceptionRecord.ExceptionCode = exception64.ExceptionCode;
        initialException.ExceptionRecord.ExceptionFlags = exception64.ExceptionFlags;
        initialException.ExceptionRecord.ExceptionRecord = (PEXCEPTION_RECORD)(ULONG_PTR)exception64.ExceptionRecord;
        initialException.ExceptionRecord.ExceptionAddress = (PVOID)(ULONG_PTR)exception64.ExceptionAddress;
        initialException.ExceptionRecord.NumberParameters = (DWORD)(std::min)(exception64.NumberParameters, (ULONG)EXCEPTION_MAXIMUM_PARAMETERS);
        for (DWORD i = 0; i < initialException.ExceptionRecord.NumberParameters; ++i)
            initialException.ExceptionRecord.ExceptionInformation[i] = (ULONG_PTR)exception64.ExceptionInformation[i];
    }

    gProcessInfo.hProcess = eventProcess;
    gProcessInfo.hThread = eventThread;
    gProcessInfo.dwProcessId = processSystemId;
    gProcessInfo.dwThreadId = threadSystemId;
    gSessionKind = detectedKind;

    // DbgEng reports a dump's stored exception but does not emit the live
    // create-process/thread/module bootstrap required by x64dbg. Discard the
    // pre-bootstrap callbacks and synthesize a deterministic snapshot.
    {
        std::lock_guard lock(gMutexCallbackQueue);
        gCallbackQueue = {};
    }
    gEventCallbacks->CreateProcess(0, (ULONG64)(uintptr_t)eventProcess, imageBase, 0, nullptr, nullptr, 0, 0,
                                   (ULONG64)(uintptr_t)eventThread, teb, instruction);

    for (ULONG i = 0; i < threadCount; ++i)
    {
        if (threadEngineIds[i] == threadIndex)
            continue;
        const auto token = createSyntheticTitanHandle(TitanHandleType::Thread, threadEngineIds[i], threadSystemIds[i], false);
        gDebugIdMap.threadIndexToHandle[threadEngineIds[i]] = token;
        gDebugIdMap.threadHandleToIndex[token] = threadEngineIds[i];

        ULONG64 threadTeb = 0;
        if (SUCCEEDED(gDebugSystemObjects->SetCurrentThreadId(threadEngineIds[i])))
            gDebugSystemObjects->GetCurrentThreadTeb(&threadTeb);
        gProcessTebCache[token] = threadTeb;
        gEventCallbacks->CreateThread((ULONG64)(uintptr_t)token, threadTeb, 0);
    }
    gDebugSystemObjects->SetCurrentThreadId(threadIndex);

    ULONG loadedModules = 0;
    ULONG unloadedModules = 0;
    if (SUCCEEDED(gDebugSymbols->GetNumberModules(&loadedModules, &unloadedModules)))
    {
        for (ULONG i = 1; i < loadedModules; ++i)
        {
            ULONG64 base = 0;
            if (SUCCEEDED(gDebugSymbols->GetModuleByIndex(i, &base)))
                gEventCallbacks->LoadModule(0, base, 0, nullptr, nullptr, 0, 0);
        }
    }
    queueCallback([initialException]
    {
        setFakeDebugEvent(initialException);
        dispatchSystemBreakpoint(&initialException);
        return true;
    });

    const auto initProcess = createSyntheticTitanHandle(TitanHandleType::Process, processIndex, processSystemId, true);
    const auto initThread = createSyntheticTitanHandle(TitanHandleType::Thread, threadIndex, threadSystemId, true);
    gDebugIdMap.processHandleToIndex[initProcess] = processIndex;
    gDebugIdMap.threadHandleToIndex[initThread] = threadIndex;
    gProcessPebCache[initProcess] = peb;
    gProcessTebCache[initThread] = teb;
    gProcessInfo.hProcess = initProcess;
    gProcessInfo.hThread = initThread;
    return &gProcessInfo;
}

__declspec(dllexport) bool GetSessionInfo(TITAN_SESSION_INFO* SessionInfo)
{
    if (!SessionInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    *SessionInfo = {};
    SessionInfo->structSize = sizeof(*SessionInfo);
    SessionInfo->kind = gSessionKind.load();
    if (SessionInfo->kind == UE_SESSION_NONE)
        return true;

    if (SessionInfo->kind == UE_SESSION_LIVE)
    {
        SessionInfo->capabilities = UE_SESSION_CAP_MEMORY_READ | UE_SESSION_CAP_MEMORY_QUERY |
                                    UE_SESSION_CAP_CONTEXT_READ | UE_SESSION_CAP_FORWARD_EXECUTION |
                                    UE_SESSION_CAP_MEMORY_WRITE | UE_SESSION_CAP_CONTEXT_WRITE |
                                    UE_SESSION_CAP_PROCESS_CONTROL | UE_SESSION_CAP_THREAD_CONTROL |
                                    UE_SESSION_CAP_NATIVE_HANDLES | UE_SESSION_CAP_EXCEPTION_CONTINUE;
    }
    else if (SessionInfo->kind == UE_SESSION_MINIDUMP)
    {
        SessionInfo->capabilities = UE_SESSION_CAP_MEMORY_READ | UE_SESSION_CAP_MEMORY_QUERY |
                                    UE_SESSION_CAP_CONTEXT_READ;
    }

#ifdef _WIN64
    SessionInfo->machineType = IMAGE_FILE_MACHINE_AMD64;
#else
    SessionInfo->machineType = IMAGE_FILE_MACHINE_I386;
#endif
    if (gDebugControl)
    {
        ULONG machineType = 0;
        if (SUCCEEDED(gDebugControl->GetEffectiveProcessorType(&machineType)))
            SessionInfo->machineType = machineType;
    }
    SessionInfo->processId = debugProcessId();
    SessionInfo->threadId = debugThreadId();
    return true;
}

__declspec(dllexport) bool StopDebug()
{
    const auto kind = gSessionKind.load();
    const auto endMode = kind == UE_SESSION_LIVE ? DEBUG_END_ACTIVE_TERMINATE : DEBUG_END_PASSIVE;
    auto hr = gDebugClient->EndSession(endMode);
    if (SUCCEEDED(hr) && !gIsDebugging && kind != UE_SESSION_LIVE)
    {
        gProcessInfo = {};
        gProcessPebCache = {};
        gProcessTebCache = {};
        closeCallerOwnedTitanHandles();
        gDebugIdMap = {};
        {
            std::lock_guard lock(gMutexCallbackQueue);
            gCallbackQueue = {};
        }
        gSessionKind = UE_SESSION_NONE;
    }
    return SUCCEEDED(hr);
}

__declspec(dllexport) void SetBPXOptions(TitanBreakpointType DefaultBreakPointType)
{
    gDefaultBreakpointType = DefaultBreakPointType;
}

__declspec(dllexport) bool IsBPXEnabled(ULONG_PTR bpxAddress)
{
    PauseLock pl;
    if (!pl)
    {
        logError("IsBPXEnabled failed to acquire pause lock");
        return false;
    }
    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.kind == BreakpointKind::Software && info.offset == bpxAddress)
            return true;
    }
    return false;
}

static HANDLE sessionProcessHandle()
{
    return gDebugIdMap.processIndexToHandle.empty() ? nullptr : gDebugIdMap.processIndexToHandle.begin()->second;
}

static bool writeNativeBreakpointBytes(const BreakpointInfo& info, bool install)
{
    auto process = sessionProcessHandle();
    if (!process || !info.patchSize)
        return false;
    SIZE_T transferred = 0;
    const auto bytes = install ? info.patchBytes : info.originalBytes;
    if (!WriteProcessMemory(process, (LPVOID)(ULONG_PTR)info.offset, bytes, info.patchSize, &transferred) ||
        transferred != info.patchSize)
        return false;
    return !!FlushInstructionCache(process, (LPCVOID)(ULONG_PTR)info.offset, info.patchSize);
}

__declspec(dllexport) bool SetBPX(ULONG_PTR bpxAddress, DWORD bpxType /* TitanSoftwareBreakpointType */, TITANCBSOFTBP bpxCallBack)
{
    if (!bpxAddress || !bpxCallBack)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    PauseLock pl;
    if (!pl && !gIsDebugging)
    {
        logError("SetBPX called without an active debug session");
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    for (const auto& [id, existing] : gBreakpoints)
    {
        if (existing.kind == BreakpointKind::Software && existing.offset == bpxAddress)
        {
            SetLastError(ERROR_ALREADY_EXISTS);
            return false;
        }
    }

    TitanBreakpointType selectedType = gDefaultBreakpointType;
    switch (bpxType & 0xF0000000)
    {
    case 0: break;
    case UE_BREAKPOINT_TYPE_INT3: selectedType = UE_BREAKPOINT_INT3; break;
    case UE_BREAKPOINT_TYPE_LONG_INT3: selectedType = UE_BREAKPOINT_LONG_INT3; break;
    case UE_BREAKPOINT_TYPE_UD2: selectedType = UE_BREAKPOINT_UD2; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    BreakpointInfo info;
    info.type = DEBUG_BREAKPOINT_CODE;
    info.callback = bpxCallBack;
    info.offset = bpxAddress;
    info.oneShot = (bpxType & UE_SINGLESHOOT) != 0;
    if (selectedType == UE_BREAKPOINT_LONG_INT3)
    {
        info.nativePatch = true;
        info.patchSize = 2;
        info.patchBytes[0] = 0xCD;
        info.patchBytes[1] = 0x03;
    }
    else if (selectedType == UE_BREAKPOINT_UD2)
    {
        info.nativePatch = true;
        info.patchSize = 2;
        info.patchBytes[0] = 0x0F;
        info.patchBytes[1] = 0x0B;
    }

    auto installNative = [&]()
    {
        auto process = sessionProcessHandle();
        if (!process)
            return false;
        if (!info.patchSize)
        {
            info.patchSize = 1;
            info.patchBytes[0] = 0xCC;
        }
        SIZE_T transferred = 0;
        if (!ReadProcessMemory(process, (LPCVOID)bpxAddress, info.originalBytes, info.patchSize, &transferred) ||
            transferred != info.patchSize)
            return false;
        info.nativePatch = true;
        info.id = gNextNativeBreakpointId++;
        return writeNativeBreakpointBytes(info, true);
    };

    // Keep software breakpoint bytes adapter-owned so MemoryReadUnsafe can
    // expose the actual patch while MemoryReadSafe can restore the captured
    // original bytes without relying on DbgEng's read filtering.
    if (!installNative())
        return false;
    gRetiredBreakpointAddresses.erase(bpxAddress);
    gBreakpoints.insert_or_assign(info.id, info);
    return true;
}

__declspec(dllexport) bool DeleteBPX(ULONG_PTR bpxAddress)
{
    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.kind == BreakpointKind::Software && info.offset == bpxAddress)
        {
            if (info.nativePatch)
            {
                if (!writeNativeBreakpointBytes(info, false))
                    return false;
            }
            else
            {
                auto hr = gDebugControl->RemoveBreakpoint2(info.bp);
                if (FAILED(hr))
                {
                    logError("Failed to remove breakpoint: {:#x}", (uint32_t)hr);
                    return false;
                }
            }
            gRetiredBreakpointAddresses.insert(bpxAddress);
            gBreakpoints.erase(id);
            return true;
        }
    }
    SetLastError(ERROR_NOT_FOUND);
    return false;
}

__declspec(dllexport) bool SetMemoryBPXEx(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory, TitanMemoryBreakpointType BreakPointType, bool RestoreOnHit, TITANCBMEMBP bpxCallBack)
{
    if (!MemoryStart || !SizeOfMemory || MemoryStart + SizeOfMemory <= MemoryStart || !bpxCallBack ||
        (BreakPointType != UE_MEMORY && BreakPointType != UE_MEMORY_READ &&
         BreakPointType != UE_MEMORY_WRITE && BreakPointType != UE_MEMORY_EXECUTE))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }
    auto process = activeProcessHandle();
    if (!process)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    for (const auto& [id, existing] : gMemoryBreakpoints)
    {
        if (existing.start == MemoryStart && existing.size == SizeOfMemory)
        {
            SetLastError(ERROR_ALREADY_EXISTS);
            return false;
        }
    }

    MemoryBreakpointInfo info;
    info.id = gNextMemoryBreakpointId++;
    info.start = MemoryStart;
    info.size = SizeOfMemory;
    info.type = BreakPointType;
    info.restoreOnHit = RestoreOnHit;
    info.callback = bpxCallBack;
    gMemoryBreakpoints.emplace(info.id, info);

    const auto startPage = pageAddress(MemoryStart);
    const auto endPage = pageAddress(MemoryStart + SizeOfMemory - 1);
    std::vector<ULONG_PTR> touchedPages;
    bool success = true;
    for (auto page = startPage;; page += targetPageSize())
    {
        auto existingPage = gMemoryBreakpointPages.find(page);
        if (existingPage != gMemoryBreakpointPages.end())
        {
            existingPage->second.breakpointIds.insert(info.id);
            touchedPages.push_back(page);
        }
        else
        {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQueryEx(process, (LPCVOID)page, &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT || mbi.Protect == 0)
            {
                success = false;
                SetLastError(ERROR_INVALID_ADDRESS);
                break;
            }

            MemoryBreakpointPage pageInfo;
            pageInfo.originalProtect = mbi.Protect;
            pageInfo.breakpointIds.insert(info.id);
            gMemoryBreakpointPages.emplace(page, std::move(pageInfo));
            touchedPages.push_back(page);
            if (!protectMemoryBreakpointPage(page, true))
            {
                success = false;
                break;
            }
        }
        if (page == endPage)
            break;
    }

    if (!success)
    {
        for (const auto page : touchedPages)
        {
            auto found = gMemoryBreakpointPages.find(page);
            if (found == gMemoryBreakpointPages.end())
                continue;
            found->second.breakpointIds.erase(info.id);
            if (found->second.breakpointIds.empty())
            {
                protectMemoryBreakpointPage(page, false);
                gMemoryBreakpointPages.erase(found);
            }
        }
        gMemoryBreakpoints.erase(info.id);
        return false;
    }
    return true;
}

__declspec(dllexport) bool RemoveMemoryBPX(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory)
{
    if (!MemoryStart || !SizeOfMemory)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }

    for (const auto& [id, info] : gMemoryBreakpoints)
    {
        if (info.start == MemoryStart && info.size == SizeOfMemory)
            return removeMemoryBreakpointById(id);
    }
    SetLastError(ERROR_NOT_FOUND);
    return false;
}

static void readReplayExtendedContext(TITAN_ENGINE_CONTEXT_t* titcontext)
{
    titcontext->x87fpu.ControlWord = (WORD)gRegisterCache.GetValue(UE_X87_CONTROLWORD);
    titcontext->x87fpu.StatusWord = (WORD)gRegisterCache.GetValue(UE_X87_STATUSWORD);
    titcontext->x87fpu.TagWord = (WORD)gRegisterCache.GetValue(UE_X87_TAGWORD);
    titcontext->MxCsr = (DWORD)gRegisterCache.GetValue(UE_MXCSR);

    char name[16] = {};
    for (size_t i = 0; i < 8; ++i)
    {
        std::snprintf(name, sizeof(name), "st%zu", i);
        if (const auto* value = gRegisterCache.FindValue(name))
        {
            if (value->Type == DEBUG_VALUE_FLOAT80 || value->Type == DEBUG_VALUE_FLOAT82)
                memcpy(titcontext->RegisterArea + i * 10, value->F80Bytes, 10);
        }
    }
    for (size_t i = 0; i < std::size(titcontext->XmmRegisters); ++i)
    {
        std::snprintf(name, sizeof(name), "xmm%zu", i);
        if (const auto* value = gRegisterCache.FindValue(name))
        {
            if (value->Type == DEBUG_VALUE_VECTOR128)
                memcpy(&titcontext->XmmRegisters[i], value->VI8, sizeof(XmmRegister_t));
        }
        titcontext->YmmRegisters[i].Low = titcontext->XmmRegisters[i];
        std::snprintf(name, sizeof(name), "ymm%zu", i);
        if (const auto* value = gRegisterCache.FindValue(name))
        {
            if (value->Type == DEBUG_VALUE_VECTOR128)
                memcpy(&titcontext->YmmRegisters[i].High, value->VI8, sizeof(XmmRegister_t));
        }
    }
}

__declspec(dllexport) bool GetFullContextDataEx(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    if (!titcontext)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    // Zero out the context structure to avoid uninitialized data
    *titcontext = {};

    PauseLock pl;
    if (!pl)
    {
        return false;
    }

    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return false;

#ifdef _WIN64
    titcontext->cax = gRegisterCache.GetValue(UE_RAX);
    titcontext->cbx = gRegisterCache.GetValue(UE_RBX);
    titcontext->ccx = gRegisterCache.GetValue(UE_RCX);
    titcontext->cdx = gRegisterCache.GetValue(UE_RDX);
    titcontext->csi = gRegisterCache.GetValue(UE_RSI);
    titcontext->cdi = gRegisterCache.GetValue(UE_RDI);
    titcontext->cbp = gRegisterCache.GetValue(UE_RBP);
    titcontext->csp = gRegisterCache.GetValue(UE_RSP);
    titcontext->cip = gRegisterCache.GetValue(UE_RIP);
    titcontext->eflags = gRegisterCache.GetValue(UE_RFLAGS);
    titcontext->r8 = gRegisterCache.GetValue(UE_R8);
    titcontext->r9 = gRegisterCache.GetValue(UE_R9);
    titcontext->r10 = gRegisterCache.GetValue(UE_R10);
    titcontext->r11 = gRegisterCache.GetValue(UE_R11);
    titcontext->r12 = gRegisterCache.GetValue(UE_R12);
    titcontext->r13 = gRegisterCache.GetValue(UE_R13);
    titcontext->r14 = gRegisterCache.GetValue(UE_R14);
    titcontext->r15 = gRegisterCache.GetValue(UE_R15);
#else
    titcontext->cax = gRegisterCache.GetValue(UE_EAX);
    titcontext->cbx = gRegisterCache.GetValue(UE_EBX);
    titcontext->ccx = gRegisterCache.GetValue(UE_ECX);
    titcontext->cdx = gRegisterCache.GetValue(UE_EDX);
    titcontext->csi = gRegisterCache.GetValue(UE_ESI);
    titcontext->cdi = gRegisterCache.GetValue(UE_EDI);
    titcontext->cbp = gRegisterCache.GetValue(UE_EBP);
    titcontext->csp = gRegisterCache.GetValue(UE_ESP);
    titcontext->cip = gRegisterCache.GetValue(UE_EIP);
    titcontext->eflags = gRegisterCache.GetValue(UE_EFLAGS);
#endif
    titcontext->cs = (WORD)gRegisterCache.GetValue(UE_SEG_CS);
    titcontext->ss = (WORD)gRegisterCache.GetValue(UE_SEG_SS);
    titcontext->ds = (WORD)gRegisterCache.GetValue(UE_SEG_DS);
    titcontext->es = (WORD)gRegisterCache.GetValue(UE_SEG_ES);
    titcontext->fs = (WORD)gRegisterCache.GetValue(UE_SEG_FS);
    titcontext->gs = (WORD)gRegisterCache.GetValue(UE_SEG_GS);
    titcontext->dr0 = gRegisterCache.GetValue(UE_DR0);
    titcontext->dr1 = gRegisterCache.GetValue(UE_DR1);
    titcontext->dr2 = gRegisterCache.GetValue(UE_DR2);
    titcontext->dr3 = gRegisterCache.GetValue(UE_DR3);
    titcontext->dr6 = gRegisterCache.GetValue(UE_DR6);
    titcontext->dr7 = gRegisterCache.GetValue(UE_DR7);
    titcontext->MxCsr = (DWORD)gRegisterCache.GetValue(UE_MXCSR);

    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        readReplayExtendedContext(titcontext);
        return true;
    }

    NativeContextBuffer native;
    if (!native.Load(hActiveThread, XSTATE_MASK_AVX))
    {
        logError("GetThreadContext with AVX state failed: {:#x}", (uint32_t)GetLastError());
        return false;
    }
    readExtendedContext(native, titcontext);

    return true;
}

__declspec(dllexport) bool SetFullContextDataEx(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    if (!titcontext)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    PauseLock pl;
    if (!pl)
        return false;
    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return false;

    std::vector<std::pair<TitanRegister, ULONG_PTR>> values = {
#ifdef _WIN64
        { UE_RAX, titcontext->cax }, { UE_RBX, titcontext->cbx }, { UE_RCX, titcontext->ccx },
        { UE_RDX, titcontext->cdx }, { UE_RSI, titcontext->csi }, { UE_RDI, titcontext->cdi },
        { UE_RBP, titcontext->cbp }, { UE_RSP, titcontext->csp }, { UE_RIP, titcontext->cip },
        { UE_RFLAGS, titcontext->eflags }, { UE_R8, titcontext->r8 }, { UE_R9, titcontext->r9 },
        { UE_R10, titcontext->r10 }, { UE_R11, titcontext->r11 }, { UE_R12, titcontext->r12 },
        { UE_R13, titcontext->r13 }, { UE_R14, titcontext->r14 }, { UE_R15, titcontext->r15 },
#else
        { UE_EAX, titcontext->cax }, { UE_EBX, titcontext->cbx }, { UE_ECX, titcontext->ccx },
        { UE_EDX, titcontext->cdx }, { UE_ESI, titcontext->csi }, { UE_EDI, titcontext->cdi },
        { UE_EBP, titcontext->cbp }, { UE_ESP, titcontext->csp }, { UE_EIP, titcontext->cip },
        { UE_EFLAGS, titcontext->eflags },
#endif
        { UE_SEG_CS, titcontext->cs }, { UE_SEG_SS, titcontext->ss }, { UE_SEG_DS, titcontext->ds },
        { UE_SEG_ES, titcontext->es }, { UE_SEG_FS, titcontext->fs }, { UE_SEG_GS, titcontext->gs },
        { UE_DR0, titcontext->dr0 }, { UE_DR1, titcontext->dr1 }, { UE_DR2, titcontext->dr2 },
        { UE_DR3, titcontext->dr3 }, { UE_DR6, titcontext->dr6 }, { UE_DR7, titcontext->dr7 },
        { UE_MXCSR, titcontext->MxCsr },
    };
    for (const auto& [reg, value] : values)
    {
        if (!gRegisterCache.SetValue(reg, value))
            return false;
    }
    if (!gRegisterCache.Flush())
        return false;

    NativeContextBuffer native;
    if (!native.Load(hActiveThread, XSTATE_MASK_AVX))
    {
        logError("GetThreadContext before extended-context write failed: {:#x}", (uint32_t)GetLastError());
        return false;
    }
    writeExtendedContext(native, titcontext);
    if (!SetThreadContext(hActiveThread, native.context))
    {
        logError("SetThreadContext with AVX state failed: {:#x}", (uint32_t)GetLastError());
        return false;
    }
    if (!gRegisterCache.Read())
        return false;
    return true;
}

__declspec(dllexport) ULONG_PTR GetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister)
{
    PauseLock pl;
    if (!pl)
    {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_ALL;
        if (!GetThreadContext(hActiveThread, &context))
            return 0;
        switch (IndexOfRegister)
        {
#ifdef _WIN64
        case UE_RAX: return context.Rax;
        case UE_RBX: return context.Rbx;
        case UE_RCX: return context.Rcx;
        case UE_RDX: return context.Rdx;
        case UE_RSI: return context.Rsi;
        case UE_RDI: return context.Rdi;
        case UE_RBP: return context.Rbp;
        case UE_RSP:
        case UE_CSP: return context.Rsp;
        case UE_RIP:
        case UE_CIP: return context.Rip;
        case UE_RFLAGS: return context.EFlags;
        case UE_R8: return context.R8;
        case UE_R9: return context.R9;
        case UE_R10: return context.R10;
        case UE_R11: return context.R11;
        case UE_R12: return context.R12;
        case UE_R13: return context.R13;
        case UE_R14: return context.R14;
        case UE_R15: return context.R15;
#else
        case UE_EAX: return context.Eax;
        case UE_EBX: return context.Ebx;
        case UE_ECX: return context.Ecx;
        case UE_EDX: return context.Edx;
        case UE_ESI: return context.Esi;
        case UE_EDI: return context.Edi;
        case UE_EBP: return context.Ebp;
        case UE_ESP:
        case UE_CSP: return context.Esp;
        case UE_EIP:
        case UE_CIP: return context.Eip;
        case UE_EFLAGS: return context.EFlags;
#endif
        default: return 0;
        }
    }

    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return 0;
    return gRegisterCache.GetValue(IndexOfRegister);
}

__declspec(dllexport) bool SetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister, ULONG_PTR NewRegisterValue)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    PauseLock pl;
    if (!pl)
        return false;
    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope || !gRegisterCache.SetValue(IndexOfRegister, NewRegisterValue))
        return false;
    return gRegisterCache.Flush();
}

__declspec(dllexport) bool GetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    return GetFullContextDataEx(hActiveThread, titcontext);
}

__declspec(dllexport) bool SetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    return SetFullContextDataEx(hActiveThread, titcontext);
}

__declspec(dllexport) bool GetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    if (!titcontext)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    *titcontext = {};

    TITAN_ENGINE_CONTEXT_t avx = {};
    if (!GetAVXContext(hActiveThread, &avx))
        return false;
    for (size_t i = 0; i < std::size(avx.YmmRegisters); ++i)
        titcontext->ZmmRegisters[i].Low = avx.YmmRegisters[i];

    PauseLock pl;
    if (!pl)
        return false;
    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return false;

    const auto enabled = GetEnabledXStateFeatures();
    if ((enabled & XSTATE_MASK_AVX512) != XSTATE_MASK_AVX512)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    NativeContextBuffer native;
    if (!native.Load(hActiveThread, XSTATE_MASK_AVX | XSTATE_MASK_AVX512))
        return false;

    constexpr size_t registerCount = std::size(titcontext->ZmmRegisters);
    auto opmask = static_cast<const ULONGLONG*>(native.Feature(XSTATE_AVX512_KMASK, sizeof(titcontext->Opmask)));
    auto zmmHigh = static_cast<const YmmRegister_t*>(native.Feature(XSTATE_AVX512_ZMM_H, std::min<size_t>(registerCount, 16) * sizeof(YmmRegister_t)));
#ifdef _WIN64
    auto zmm16 = static_cast<const ZmmRegister_t*>(native.Feature(XSTATE_AVX512_ZMM, 16 * sizeof(ZmmRegister_t)));
#endif
    if (!opmask || !zmmHigh
#ifdef _WIN64
        || !zmm16
#endif
    )
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    memcpy(titcontext->Opmask, opmask, sizeof(titcontext->Opmask));
    for (size_t i = 0; i < std::min<size_t>(registerCount, 16); ++i)
        titcontext->ZmmRegisters[i].High = zmmHigh[i];
#ifdef _WIN64
    for (size_t i = 0; i < 16; ++i)
        titcontext->ZmmRegisters[i + 16] = zmm16[i];
#endif
    return true;
}

__declspec(dllexport) bool SetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    if (!titcontext)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    TITAN_ENGINE_CONTEXT_t avx = {};
    if (!GetAVXContext(hActiveThread, &avx))
        return false;
    for (size_t i = 0; i < std::size(avx.YmmRegisters); ++i)
        avx.YmmRegisters[i] = titcontext->ZmmRegisters[i].Low;
    if (!SetAVXContext(hActiveThread, &avx))
        return false;

    PauseLock pl;
    if (!pl)
        return false;
    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return false;
    const auto enabled = GetEnabledXStateFeatures();
    if ((enabled & XSTATE_MASK_AVX512) != XSTATE_MASK_AVX512)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    NativeContextBuffer native;
    if (!native.Load(hActiveThread, XSTATE_MASK_AVX | XSTATE_MASK_AVX512))
        return false;

    constexpr size_t registerCount = std::size(titcontext->ZmmRegisters);
    auto opmask = static_cast<ULONGLONG*>(native.Feature(XSTATE_AVX512_KMASK, sizeof(titcontext->Opmask)));
    auto zmmHigh = static_cast<YmmRegister_t*>(native.Feature(XSTATE_AVX512_ZMM_H, std::min<size_t>(registerCount, 16) * sizeof(YmmRegister_t)));
#ifdef _WIN64
    auto zmm16 = static_cast<ZmmRegister_t*>(native.Feature(XSTATE_AVX512_ZMM, 16 * sizeof(ZmmRegister_t)));
#endif
    if (!opmask || !zmmHigh
#ifdef _WIN64
        || !zmm16
#endif
    )
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    memcpy(opmask, titcontext->Opmask, sizeof(titcontext->Opmask));
    for (size_t i = 0; i < std::min<size_t>(registerCount, 16); ++i)
        zmmHigh[i] = titcontext->ZmmRegisters[i].High;
#ifdef _WIN64
    for (size_t i = 0; i < 16; ++i)
        zmm16[i] = titcontext->ZmmRegisters[i + 16];
#endif
    if (!SetThreadContext(hActiveThread, native.context))
        return false;
    return gRegisterCache.Read();
}

__declspec(dllexport) bool Fill(LPVOID MemoryStart, DWORD MemorySize, PBYTE FillByte)
{
    if (!MemoryStart || !MemorySize)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    PauseLock pl;
    if (!pl)
    {
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }
    auto process = activeProcessHandle();
    if (!process)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    const BYTE value = FillByte ? *FillByte : 0x90;
    std::vector<BYTE> chunk(std::min<DWORD>(MemorySize, 64 * 1024), value);
    SIZE_T offset = 0;
    while (offset < MemorySize)
    {
        const auto count = std::min<SIZE_T>(chunk.size(), MemorySize - offset);
        SIZE_T written = 0;
        if (!MemoryWriteSafe(process, (BYTE*)MemoryStart + offset, chunk.data(), count, &written) || written != count)
            return false;
        offset += count;
    }
    return true;
}

__declspec(dllexport) const DEBUG_EVENT* GetDebugData()
{
    // TitanEngine exposes one process-global DEBUG_EVENT snapshot. x64dbg reads
    // it from GUI/script worker threads while the debug loop is paused.
    return &gFakeDebugEvent;
}

__declspec(dllexport) void SetCustomHandler(TitanCustomHandler ExceptionId, TITANCALLBACKARG CallBack)
{
    gCustomHandlers[ExceptionId] = CallBack;
}

static void debugStep(ULONG status, TITANCBSTEP callback)
{
    PauseLock pl;
    if (!pl)
    {
        logError("debugStep({}) failed to acquire pause lock", formatSingleFlag(statusFlags, status));
        return;
    }

    ULONG threadIndex = 0;
    auto hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
    if (FAILED(hr))
    {
        logError("debugStep({}) Failed to get current thread index: {:#x}", formatSingleFlag(statusFlags, status), (uint32_t)hr);
        return;
    }

    if (gStepCallbacks.contains(threadIndex))
    {
        logError("debugStep({}) called while another step is in progress on this thread", formatSingleFlag(statusFlags, status));
        return;
    }

    gNextExecutionStatus = status;

    DWORD threadSystemId = 0;
    gDebugSystemObjects->GetCurrentThreadSystemId(&threadSystemId);
    gStepCallbacks.emplace(threadIndex, callback);
    gStepThreadSystemIds[threadIndex] = threadSystemId;
    gStepStatuses.emplace(threadIndex, status);
}

__declspec(dllexport) void StepInto(TITANCBSTEP traceCallBack)
{
    debugStep(DEBUG_STATUS_STEP_INTO, traceCallBack);
}

__declspec(dllexport) void StepOver(TITANCBSTEP traceCallBack)
{
    debugStep(DEBUG_STATUS_STEP_OVER, traceCallBack);
}

static constexpr DWORD hardwareRegisters[] = { UE_DR0, UE_DR1, UE_DR2, UE_DR3 };

static bool isHardwareRegister(DWORD value)
{
    return std::find(std::begin(hardwareRegisters), std::end(hardwareRegisters), value) != std::end(hardwareRegisters);
}

__declspec(dllexport) bool GetUnusedHardwareBreakPointRegister(LPDWORD RegisterIndex)
{
    if (!RegisterIndex)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        *RegisterIndex = 0;
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }

    for (const auto reg : hardwareRegisters)
    {
        if (!gHardwareBreakpointIds.contains(reg))
        {
            *RegisterIndex = reg;
            return true;
        }
    }

    *RegisterIndex = 0;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return false;
}

__declspec(dllexport) bool SetHardwareBreakPoint(ULONG_PTR bpxAddress, DWORD IndexOfRegister, TitanHardwareBreakpointType bpxType, TitanHardwareBreakpointSize bpxSize, TITANCBHWBP bpxCallBack)
{
    if (!bpxAddress || !bpxCallBack)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }

    if (IndexOfRegister == 0)
    {
        for (const auto reg : hardwareRegisters)
        {
            if (!gHardwareBreakpointIds.contains(reg))
            {
                IndexOfRegister = reg;
                break;
            }
        }
    }
    if (!isHardwareRegister(IndexOfRegister))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (gHardwareBreakpointIds.contains(IndexOfRegister))
    {
        SetLastError(ERROR_ALREADY_EXISTS);
        return false;
    }

    ULONG dataSize = 0;
    switch (bpxSize)
    {
    case UE_HARDWARE_SIZE_1: dataSize = 1; break;
    case UE_HARDWARE_SIZE_2: dataSize = 2; break;
    case UE_HARDWARE_SIZE_4: dataSize = 4; break;
    case UE_HARDWARE_SIZE_8: dataSize = 8; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    ULONG access = 0;
    switch (bpxType)
    {
    case UE_HARDWARE_EXECUTE:
        access = DEBUG_BREAK_EXECUTE;
        dataSize = 1;
        break;
    case UE_HARDWARE_WRITE:
        access = DEBUG_BREAK_WRITE;
        break;
    case UE_HARDWARE_READWRITE:
        access = DEBUG_BREAK_READ | DEBUG_BREAK_WRITE;
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if ((bpxAddress & (dataSize - 1)) != 0)
    {
        SetLastError(ERROR_MAPPED_ALIGNMENT);
        return false;
    }

    BreakpointInfo info;
    info.kind = BreakpointKind::Hardware;
    // DbgEng code breakpoints are required for execute access. Data
    // breakpoints with DEBUG_BREAK_EXECUTE are accepted by SetDataParameters
    // on AMD64 but are not armed by the user-mode engine. Keep Titan's DR slot
    // accounting while using DbgEng's reliable execute-breakpoint primitive.
    info.type = bpxType == UE_HARDWARE_EXECUTE ? DEBUG_BREAKPOINT_CODE : DEBUG_BREAKPOINT_DATA;
    info.offset = bpxAddress;
    info.hardwareCallback = bpxCallBack;
    info.hardwareRegister = IndexOfRegister;

    if (bpxType == UE_HARDWARE_EXECUTE)
    {
        auto process = sessionProcessHandle();
        SIZE_T transferred = 0;
        info.nativePatch = true;
        info.patchSize = 1;
        info.patchBytes[0] = 0xCC;
        info.id = gNextNativeBreakpointId++;
        if (!process ||
            !ReadProcessMemory(process, (LPCVOID)bpxAddress, info.originalBytes, 1, &transferred) || transferred != 1 ||
            !writeNativeBreakpointBytes(info, true))
        {
            return false;
        }
        gBreakpoints.insert_or_assign(info.id, info);
        gHardwareBreakpointIds.emplace(IndexOfRegister, info.id);
        return true;
    }

    auto hr = gDebugControl->AddBreakpoint2(info.type, DEBUG_ANY_ID, &info.bp);
    if (SUCCEEDED(hr))
        hr = info.bp->SetOffset(bpxAddress);
    if (SUCCEEDED(hr) && info.type == DEBUG_BREAKPOINT_DATA)
        hr = info.bp->SetDataParameters(dataSize, access);
    if (SUCCEEDED(hr))
        hr = info.bp->SetFlags(DEBUG_BREAKPOINT_ENABLED);
    if (SUCCEEDED(hr))
        hr = info.bp->GetId(&info.id);
    if (FAILED(hr))
    {
        logError("Failed to create hardware breakpoint at {:#x}: {:#x}", bpxAddress, (uint32_t)hr);
        if (info.bp)
            gDebugControl->RemoveBreakpoint2(info.bp);
        SetLastError(HRESULT_CODE(hr) ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
        return false;
    }

    ULONG verifySize = dataSize;
    ULONG verifyAccess = access;
    ULONG verifyFlags = 0;
    const auto dataHr = info.type == DEBUG_BREAKPOINT_DATA
                            ? info.bp->GetDataParameters(&verifySize, &verifyAccess)
                            : S_OK;
    const auto flagsHr = info.bp->GetFlags(&verifyFlags);
    logDebug("Hardware breakpoint {} (id {}, engine type {}) configured at {:#x}: size {}, access {:#x}, flags {:#x}, verify {:#x}/{:#x}",
             IndexOfRegister, info.id, info.type, bpxAddress, verifySize, verifyAccess, verifyFlags,
             (uint32_t)dataHr, (uint32_t)flagsHr);

    // DbgEng may recycle an ID from an already-consumed one-shot code
    // breakpoint. Replace the stale adapter record with the new active object.
    gBreakpoints.insert_or_assign(info.id, info);
    gHardwareBreakpointIds.emplace(IndexOfRegister, info.id);
    return true;
}

__declspec(dllexport) bool DeleteHardwareBreakPoint(DWORD IndexOfRegister)
{
    if (!isHardwareRegister(IndexOfRegister))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    PauseLock pl;
    if (!pl)
    {
        SetLastError(gIsDebugging ? ERROR_BUSY : ERROR_INVALID_HANDLE);
        return false;
    }

    auto slot = gHardwareBreakpointIds.find(IndexOfRegister);
    if (slot == gHardwareBreakpointIds.end())
    {
        SetLastError(ERROR_NOT_FOUND);
        return false;
    }
    auto breakpoint = gBreakpoints.find(slot->second);
    if (breakpoint == gBreakpoints.end())
    {
        gHardwareBreakpointIds.erase(slot);
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }

    HRESULT hr = S_OK;
    if (breakpoint->second.nativePatch)
    {
        if (!writeNativeBreakpointBytes(breakpoint->second, false))
            hr = HRESULT_FROM_WIN32(GetLastError());
        else
            gRetiredBreakpointAddresses.insert(breakpoint->second.offset);
    }
    else
    {
        hr = gDebugControl->RemoveBreakpoint2(breakpoint->second.bp);
    }
    if (FAILED(hr))
    {
        logError("Failed to remove hardware breakpoint in {}: {:#x}", IndexOfRegister, (uint32_t)hr);
        SetLastError(HRESULT_CODE(hr) ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
        return false;
    }
    gBreakpoints.erase(breakpoint);
    gHardwareBreakpointIds.erase(slot);
    return true;
}

__declspec(dllexport) bool RemoveAllBreakPoints(TitanBreakpointRemoveOption RemoveOption)
{
    PauseLock pl;
    if (!pl)
    {
        if (!gIsDebugging)
        {
            gBreakpoints.clear();
            gHardwareBreakpointIds.clear();
            gMemoryBreakpoints.clear();
            gMemoryBreakpointPages.clear();
            gRetiredBreakpointAddresses.clear();
            return true;
        }
        logError("RemoveAllBreakPoints failed to acquire pause lock");
        return false;
    }

    if (!removeAllMemoryBreakpoints())
        return false;

    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.nativePatch)
        {
            if (!writeNativeBreakpointBytes(info, false))
                return false;
        }
        else
        {
            auto hr = gDebugControl->RemoveBreakpoint2(info.bp);
            if (FAILED(hr))
            {
                logError("Failed to remove breakpoint {}: {:#x}", id, (uint32_t)hr);
                return false;
            }
        }
    }
    gBreakpoints.clear();
    gHardwareBreakpointIds.clear();
    gRetiredBreakpointAddresses.clear();

    return true;
}

__declspec(dllexport) void DebugLoop()
{
    if (!IsDebugThread())
    {
        logError("DebugLoop called from a thread other than InitDebugW");
        SetLastError(ERROR_INVALID_THREAD_ID);
        return;
    }
    logDebug("[{}] dwThreadId: {:#x}", __func__, GetCurrentThreadId());

    gIsDebugging = true;
    while (true)
    {
        // InitDebugW consumes the create-process event so it can return valid handles.
        // Process that queued callback before waiting for the next event.
        bool eventAlreadyQueued = false;
        {
            std::lock_guard lgQueue(gMutexCallbackQueue);
            eventAlreadyQueued = !gCallbackQueue.empty();
        }

        auto hr = S_OK;
        if (!eventAlreadyQueued)
        {
            // While waiting for an event, register and memory operations must fail
            // rather than race DbgEng's execution state.
            gPaused = false;
            ULONG execStatusBefore = 0;
            hr = gDebugControl->GetExecutionStatusEx(&execStatusBefore);
            if (FAILED(hr))
                logError("[WaitForEvent] Failed to get execution status: {:#x}", (uint32_t)hr);
            else
                logDebug("[WaitForEvent] ExecutionStatus (before): {}", formatSingleFlag(statusFlags, execStatusBefore));

            hr = gDebugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
            if (FAILED(hr))
            {
                ULONG status = DEBUG_STATUS_NO_DEBUGGEE;
                if (SUCCEEDED(gDebugControl->GetExecutionStatus(&status)) && status == DEBUG_STATUS_NO_DEBUGGEE)
                    logDebug("Debug session ended");
                else
                    logError("Failed to wait for event: {:#x}", (uint32_t)hr);
                break;
            }

            bool hasEventCallback = false;
            {
                std::lock_guard lgQueue(gMutexCallbackQueue);
                hasEventCallback = !gCallbackQueue.empty();
            }
            if (!hasEventCallback && (!gStepCallbacks.empty() || !gInternalStepCallbacks.empty()))
            {
                ULONG threadIndex = 0;
                hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
                auto internalStep = gInternalStepCallbacks.find(threadIndex);
                if (SUCCEEDED(hr) && internalStep != gInternalStepCallbacks.end())
                {
                    logDebug("Internal step completed for thread index {}", threadIndex);
                    queueCallback([threadIndex]
                    {
                        return completeInternalStep(threadIndex);
                    });
                }
                else
                {
                    auto step = gStepCallbacks.find(threadIndex);
                    if (SUCCEEDED(hr) && step != gStepCallbacks.end())
                    {
                        logDebug("Step completed for thread index {}", threadIndex);
                        const auto callback = step->second;
                        const auto threadSystemId = gStepThreadSystemIds[threadIndex];
                        gStepCallbacks.erase(step);
                        gStepThreadSystemIds.erase(threadIndex);
                        gStepStatuses.erase(threadIndex);
                        queueCallback([callback, threadSystemId]
                        {
                            gFakeDebugEvent.dwThreadId = threadSystemId;
                            gFakeDebugEvent.dwProcessId = debugProcessId();
                            gNextExecutionStatus = DEBUG_STATUS_GO;
                            callback();
                            return true;
                        });
                    }
                }
            }
        }

        ULONG execStatusAfter = 0;
        hr = gDebugControl->GetExecutionStatusEx(&execStatusAfter);
        if (FAILED(hr))
            logError("[WaitForEvent] Failed to get execution status: {:#x}", (uint32_t)hr);
        else
            logDebug("[WaitForEvent] ExecutionStatus (after): {}", formatSingleFlag(statusFlags, execStatusAfter));

        // We acquire the mutex here to ensure no other threads are using the cache
        {
            std::lock_guard lgPaused(gMutexPaused);
            if (!gRegisterCache.Read())
            {
                break;
            }
        }

        // After this point other threads will fail before trying to even acquire the lock
        gPaused = true;

        size_t callbackCount = 0;
        {
            std::lock_guard lgQueue(gMutexCallbackQueue);
            callbackCount = gCallbackQueue.size();
        }
        logDebug("Processing {} queued callbacks", callbackCount);
        while (true)
        {
            std::function<bool()> work;
            {
                std::lock_guard lgQueue(gMutexCallbackQueue);
                if (gCallbackQueue.empty())
                    break;
                work = std::move(gCallbackQueue.front());
                gCallbackQueue.pop();
            }
            if (!work())
            {
                logError("worker callback failed");
                break;
            }
        }

        {
            std::lock_guard lgPaused(gMutexPaused);
            if (!gRegisterCache.Flush())
            {
                break;
            }
        }
        if (gNextExecutionStatus != DEBUG_STATUS_NO_CHANGE)
        {
            if (gSessionKind == UE_SESSION_MINIDUMP && gNextExecutionStatus == DEBUG_STATUS_BREAK)
            {
                gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
                continue;
            }
            ULONG currentStatus = DEBUG_STATUS_NO_DEBUGGEE;
            const auto statusHr = gDebugControl->GetExecutionStatus(&currentStatus);
            if (FAILED(statusHr) || currentStatus != DEBUG_STATUS_NO_DEBUGGEE)
            {
                hr = gDebugControl->SetExecutionStatus(gNextExecutionStatus);
                if (FAILED(hr))
                    logError("SetExecutionStatus({}) failed: {:#x}", formatSingleFlag(statusFlags, gNextExecutionStatus), (uint32_t)hr);
            }
            gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
        }
    }
    gIsDebugging = false;

    // Reset caches
    gProcessInfo = {};
    gCustomHandlers = {};
    gFakeDebugEvent = {};
    {
        std::lock_guard lg(gMutexCallbackQueue);
        gCallbackQueue = {};
    }
    gProcessPebCache = {};
    gProcessTebCache = {};
    gRegisterCache = {};
    closeCallerOwnedTitanHandles();
    gDebugIdMap = {};
    gBreakpoints = {};
    gHardwareBreakpointIds = {};
    gMemoryBreakpoints = {};
    gMemoryBreakpointPages = {};
    gStepCallbacks = {};
    gStepThreadSystemIds = {};
    gInternalStepCallbacks = {};
    gInternalStepSuspendedThreads = {};
    gStepStatuses = {};
    gRetiredBreakpointAddresses = {};
    gAttachCallback = nullptr;
    gSessionKind = UE_SESSION_NONE;
}

__declspec(dllexport) void SetNextDbgContinueStatus(DWORD SetDbgCode)
{
    if (SetDbgCode != DBG_EXCEPTION_NOT_HANDLED && SetDbgCode != DBG_CONTINUE)
    {
        logError("Unsupported continue status: {:#x}", SetDbgCode);
        SetLastError(ERROR_INVALID_PARAMETER);
        return;
    }
    gNextContinueStatus = SetDbgCode;
    logDebug("Next continue status: {}", SetDbgCode == DBG_CONTINUE ? "DBG_CONTINUE" : "DBG_EXCEPTION_NOT_HANDLED");
}

__declspec(dllexport) bool AttachDebugger(DWORD ProcessId, bool KillOnExit, LPVOID DebugInfo, TITANCALLBACK CallBack)
{
    if (!ProcessId || !DebugInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    gProcessInfo = {};
    ++gSessionGeneration;
    ResetEvent(gProcessCreatedEvent);
    gDebugThreadId = GetCurrentThreadId();
    gExpectSystemBreakpoint = true;
    gAttachCallback = CallBack;
    gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
    gNextContinueStatus = DBG_CONTINUE;

    ULONG processOptions = 0;
    if (SUCCEEDED(gDebugClient->GetProcessOptions(&processOptions)))
    {
        if (KillOnExit)
            processOptions &= ~DEBUG_PROCESS_DETACH_ON_EXIT;
        else
            processOptions |= DEBUG_PROCESS_DETACH_ON_EXIT;
        gDebugClient->SetProcessOptions(processOptions);
    }

    auto hr = gDebugClient->AttachProcess(0, ProcessId, DEBUG_ATTACH_DEFAULT);
    if (FAILED(hr))
    {
        logError("AttachProcess failed: {:#x}", (uint32_t)hr);
        SetLastError(HRESULT_CODE(hr));
        gAttachCallback = nullptr;
        return false;
    }

    hr = gDebugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
    if (FAILED(hr) || WaitForSingleObject(gProcessCreatedEvent, 0) != WAIT_OBJECT_0)
    {
        logError("Failed waiting for attached create-process event: {:#x}", (uint32_t)hr);
        gDebugClient->DetachProcesses();
        gAttachCallback = nullptr;
        SetLastError(ERROR_GEN_FAILURE);
        return false;
    }

    gSessionKind = UE_SESSION_LIVE;
    *static_cast<PROCESS_INFORMATION*>(DebugInfo) = gProcessInfo;
    DebugLoop();
    return true;
}

__declspec(dllexport) bool DetachDebuggerEx(DWORD ProcessId)
{
    PauseLock pl;
    if (pl && !removeAllMemoryBreakpoints())
        return false;
    const auto hr = gDebugClient->DetachProcesses();
    if (FAILED(hr))
    {
        logError("DetachProcesses failed: {:#x}", (uint32_t)hr);
        SetLastError(HRESULT_CODE(hr));
        return false;
    }
    return true;
}

__declspec(dllexport) bool IsFileBeingDebugged()
{
    return gIsDebugging;
}

// TitanEngine.Process.functions:
__declspec(dllexport) HANDLE TitanOpenProcess(DWORD dwDesiredAccess, bool bInheritHandle, DWORD dwProcessId)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        ULONG processIndex = DEBUG_ANY_ID;
        if (dwProcessId != debugProcessId() || FAILED(gDebugSystemObjects->GetCurrentProcessId(&processIndex)))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const auto handle = createSyntheticTitanHandle(TitanHandleType::Process, processIndex, dwProcessId, true);
        gDebugIdMap.processHandleToIndex[handle] = processIndex;
        ULONG64 peb = 0;
        gDebugSystemObjects->GetCurrentProcessPeb(&peb);
        gProcessPebCache[handle] = peb;
        return handle;
    }
    auto handle = OpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
    registerTitanHandle(handle, TitanHandleType::Process, DEBUG_ANY_ID, dwProcessId, true);
    return handle;
}

__declspec(dllexport) HANDLE TitanOpenThread(DWORD dwDesiredAccess, bool bInheritHandle, DWORD dwThreadId)
{
    if (gSessionKind == UE_SESSION_MINIDUMP || gSessionKind == UE_SESSION_TTD)
    {
        ULONG count = 0;
        if (FAILED(gDebugSystemObjects->GetNumberThreads(&count)) || !count)
            return nullptr;
        std::vector<ULONG> engineIds(count);
        std::vector<ULONG> systemIds(count);
        if (FAILED(gDebugSystemObjects->GetThreadIdsByIndex(0, count, engineIds.data(), systemIds.data())))
            return nullptr;
        for (ULONG i = 0; i < count; ++i)
        {
            if (systemIds[i] != dwThreadId)
                continue;
            const auto handle = createSyntheticTitanHandle(TitanHandleType::Thread, engineIds[i], dwThreadId, true);
            gDebugIdMap.threadHandleToIndex[handle] = engineIds[i];
            ULONG oldIndex = DEBUG_ANY_ID;
            ULONG64 teb = 0;
            gDebugSystemObjects->GetCurrentThreadId(&oldIndex);
            if (SUCCEEDED(gDebugSystemObjects->SetCurrentThreadId(engineIds[i])))
                gDebugSystemObjects->GetCurrentThreadTeb(&teb);
            if (oldIndex != DEBUG_ANY_ID)
                gDebugSystemObjects->SetCurrentThreadId(oldIndex);
            gProcessTebCache[handle] = teb;
            return handle;
        }
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto handle = OpenThread(dwDesiredAccess, bInheritHandle, dwThreadId);
    registerTitanHandle(handle, TitanHandleType::Thread, DEBUG_ANY_ID, dwThreadId, true);
    return handle;
}

__declspec(dllexport) bool TitanGetProcessImagePathW(HANDLE hProcess, LPWSTR szPath, SIZE_T cchPath)
{
    if (!hProcess || !szPath || !cchPath || cchPath > ULONG_MAX ||
        !gDebugIdMap.processHandleToIndex.contains(hProcess))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    ULONG64 base = 0;
    auto hr = gDebugSymbols->GetModuleByIndex(0, &base);
    if (SUCCEEDED(hr))
        hr = gDebugSymbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, 0, base, szPath, (ULONG)cchPath, nullptr);
    if (FAILED(hr))
    {
        if (cchPath)
            *szPath = L'\0';
        SetLastError(ERROR_NOT_FOUND);
        return false;
    }
    return true;
}

__declspec(dllexport) bool TitanGetModulePathW(HANDLE hProcess, ULONG_PTR ModuleBase, LPWSTR szPath, SIZE_T cchPath)
{
    if (!hProcess || !ModuleBase || !szPath || !cchPath || cchPath > ULONG_MAX ||
        !gDebugIdMap.processHandleToIndex.contains(hProcess))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    ULONG index = DEBUG_ANY_ID;
    ULONG64 base = 0;
    auto hr = gDebugSymbols->GetModuleByOffset((ULONG64)ModuleBase, 0, &index, &base);
    if (SUCCEEDED(hr) && base == (ULONG64)ModuleBase)
        hr = gDebugSymbols->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, index, base, szPath, (ULONG)cchPath, nullptr);
    else if (SUCCEEDED(hr))
        hr = E_INVALIDARG;
    if (FAILED(hr))
    {
        if (cchPath)
            *szPath = L'\0';
        SetLastError(ERROR_NOT_FOUND);
        return false;
    }
    return true;
}

__declspec(dllexport) bool TitanCloseHandle(HANDLE hEngineHandle)
{
    std::lock_guard lock(gMutexHandleRegistry);
    const auto found = gHandleRegistry.find(hEngineHandle);
    if (found == gHandleRegistry.end() || !found->second.callerOwned)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    const auto result = !found->second.nativeHandle || !!CloseHandle(found->second.nativeHandle);
    if (result)
    {
        gDebugIdMap.processHandleToIndex.erase(hEngineHandle);
        gDebugIdMap.threadHandleToIndex.erase(hEngineHandle);
        gProcessPebCache.erase(hEngineHandle);
        gProcessTebCache.erase(hEngineHandle);
        gHandleRegistry.erase(found);
    }
    return result;
}

__declspec(dllexport) bool ProcessIsWow64(HANDLE hProcess, PBOOL isWow64)
{
    if (!isWow64)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto nativeHandle = registeredNativeHandle(hProcess, TitanHandleType::Process);
    return nativeHandle && !!IsWow64Process(nativeHandle, isWow64);
}

__declspec(dllexport) bool TitanTerminateProcess(HANDLE hProcess, DWORD exitCode) { const auto h = registeredNativeHandle(hProcess, TitanHandleType::Process); return h && !!TerminateProcess(h, exitCode); }
__declspec(dllexport) bool TitanDebugBreakProcess(HANDLE hProcess) { const auto h = registeredNativeHandle(hProcess, TitanHandleType::Process); return h && !!DebugBreakProcess(h); }
__declspec(dllexport) HANDLE TitanCreateRemoteThread(HANDLE hProcess, LPTHREAD_START_ROUTINE start, LPVOID argument, DWORD creationFlags, LPDWORD threadId)
{
    const auto process = registeredNativeHandle(hProcess, TitanHandleType::Process);
    if (!process)
        return nullptr;
    auto thread = CreateRemoteThread(process, nullptr, 0, start, argument, creationFlags, threadId);
    registerTitanHandle(thread, TitanHandleType::Thread, DEBUG_ANY_ID, threadId ? *threadId : 0, true);
    return thread;
}
__declspec(dllexport) DWORD TitanSuspendThread(HANDLE hThread) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h ? SuspendThread(h) : (DWORD)-1; }
__declspec(dllexport) DWORD TitanResumeThread(HANDLE hThread) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h ? ResumeThread(h) : (DWORD)-1; }
__declspec(dllexport) bool TitanTerminateThread(HANDLE hThread, DWORD exitCode) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h && !!TerminateThread(h, exitCode); }
__declspec(dllexport) DWORD TitanGetThreadId(HANDLE hThread)
{
    std::lock_guard lock(gMutexHandleRegistry);
    const auto found = gHandleRegistry.find(hThread);
    if (found == gHandleRegistry.end() || found->second.type != TitanHandleType::Thread)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return found->second.nativeHandle ? GetThreadId(found->second.nativeHandle) : found->second.systemId;
}
__declspec(dllexport) int TitanGetThreadPriority(HANDLE hThread) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h ? GetThreadPriority(h) : THREAD_PRIORITY_ERROR_RETURN; }
__declspec(dllexport) bool TitanSetThreadPriority(HANDLE hThread, int priority) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h && !!SetThreadPriority(h, priority); }
__declspec(dllexport) bool TitanGetThreadTimes(HANDLE hThread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h && !!GetThreadTimes(h, creation, exit, kernel, user); }
__declspec(dllexport) bool TitanQueryThreadCycleTime(HANDLE hThread, PULONG64 cycleTime) { const auto h = registeredNativeHandle(hThread, TitanHandleType::Thread); return h && !!QueryThreadCycleTime(h, cycleTime); }

static std::map<ULONG_PTR, FileMap<uint8_t>*> gMappedFiles;

// TitanEngine.StaticUnpacker.functions:
__declspec(dllexport) bool StaticFileLoadW(const wchar_t* szFileName, DWORD DesiredAccess, bool SimulateLoad, LPHANDLE FileHandle, LPDWORD LoadedSize, LPHANDLE FileMap, PULONG_PTR FileMapVA)
{
    if (SimulateLoad)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }

    auto file = new ::FileMap<uint8_t>;
    if (!file->Map(szFileName, DesiredAccess == UE_ACCESS_ALL))
    {
        delete file;
        return false;
    }
    *FileHandle = file->hFile;
    *LoadedSize = file->size;
    *FileMap = file->hMap;
    *FileMapVA = ULONG_PTR(file->data);
    gMappedFiles.insert({ *FileMapVA, file });
    return true;
}

__declspec(dllexport) bool StaticFileUnloadW(const wchar_t* szFileName, bool CommitChanges, HANDLE FileHandle, DWORD LoadedSize, HANDLE FileMap, ULONG_PTR FileMapVA)
{
    auto found = gMappedFiles.find(FileMapVA);
    if (found == gMappedFiles.end())
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    // HACK: x64dbg breaks the API here
    if (FileHandle == (HANDLE)1)
    {
        found->second->hFile = INVALID_HANDLE_VALUE;
    }
    delete found->second;
    gMappedFiles.erase(found);
    return true;
}

// TitanEngine.Engine.functions:
static bool setDebugPrivilege(bool enabled)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid = {};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid))
    {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
    SetLastError(ERROR_SUCCESS);
    const auto result = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const auto error = GetLastError();
    CloseHandle(token);
    SetLastError(error);
    return result && error == ERROR_SUCCESS;
}

__declspec(dllexport) void SetEngineVariable(TitanEngineVariable VariableId, bool VariableSet)
{
    gEngineVariables[VariableId] = VariableSet;
    if (VariableId == UE_ENGINE_SET_DEBUG_PRIVILEGE && !setDebugPrivilege(VariableSet) &&
        GetLastError() != ERROR_NOT_ALL_ASSIGNED)
        logError("Failed to {} debug privilege: {:#x}", VariableSet ? "enable" : "disable", (uint32_t)GetLastError());
}

__declspec(dllexport) bool EngineCheckStructAlignment(TitanStructureType StructureType, ULONG_PTR StructureSize)
{
    // NOTE: This is called when x64dbg starts, so we can do initialization here
    if (!InitializeDbgEng())
        return false;

    if (StructureType == UE_STRUCT_TITAN_ENGINE_CONTEXT)
    {
        if (StructureSize != sizeof(TITAN_ENGINE_CONTEXT_t))
            logError("TITAN_ENGINE_CONTEXT_t size mismatch: caller {}, adapter {}", StructureSize, sizeof(TITAN_ENGINE_CONTEXT_t));
        return StructureSize == sizeof(TITAN_ENGINE_CONTEXT_t);
    }
    return false;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (gDbgEngThread)
        {
            SetEvent(gDbgEngEvent);
            WaitForSingleObject(gDbgEngThread, INFINITE);
            CloseHandle(gDbgEngThread);
            CloseHandle(gDbgEngEvent);
            CloseHandle(gProcessCreatedEvent);
            gDbgEngThread = nullptr;
            gDbgEngEvent = nullptr;
            gProcessCreatedEvent = nullptr;
        }
    }
    return TRUE;
}
