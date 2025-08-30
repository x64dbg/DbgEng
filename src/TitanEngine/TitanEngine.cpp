#include <map>
#include <format>
#include <vector>
#include <mutex>
#include <queue>
#include <functional>
#include <atomic>

#include "TitanEngine.h"
#include "FileMap.h"

#include <DbgEng.h>
#include <delayimp.h>
#include <atlbase.h>
using namespace ATL;

static decltype(&printf) _plugin_logprintf;

static void x64dbgLog(std::string&& line)
{
    line += "\n";
    _plugin_logprintf("%s", line.c_str());
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

struct BreakpointInfo
{
    IDebugBreakpoint2* bp = nullptr;
    ULONG id = 0;
    ULONG type = 0;
    DWORD64 offset = 0;
    TITANCBSOFTBP callback = nullptr;
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
static std::map<ULONG, BreakpointInfo> gBreakpoints;
static std::recursive_mutex gMutexPaused;
static std::atomic_bool gPaused; // this is set to true when we are not inside WaitForEvent (perhaps it should be renamed?)
static std::atomic_bool gIsDebugging;
static bool gDbgEngInitialized = false;

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

static IDebugClient9* gDebugClient = nullptr;

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
static ULONG gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;

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
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

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

        auto work = [=]
        {
            auto itr = gBreakpoints.find(id);
            if (itr == gBreakpoints.end())
            {
                logError("Breakpoint id {} not found in map", id);
                return false;
            }

            // TODO: support other breakpoint types
            EXCEPTION_DEBUG_INFO info = {};
            info.ExceptionRecord.ExceptionCode = EXCEPTION_BREAKPOINT;
            info.ExceptionRecord.ExceptionFlags = 0;
            info.ExceptionRecord.ExceptionAddress = (PVOID)offset;
            setFakeDebugEvent(info);

            const auto& bpInfo = itr->second;
            if (bpInfo.callback)
            {
                bpInfo.callback();
            }
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(Exception)(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

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
            gCustomHandlers.at(UE_CH_UNHANDLEDEXCEPTION)(&info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateThread)(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

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
                // NOTE: sanity check
                ULONG handleIndex = 0;
                hr = gDebugSystemObjects->GetThreadIdByHandle(Handle, &handleIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event thread index by handle");
                    handleIndex = -1;
                }
            }
            setFakeDebugEvent(info);
            gCustomHandlers.at(UE_CH_CREATETHREAD)(&info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(ExitThread)(ULONG ExitCode) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

        logDebug("[{}] Thread exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);

        auto work = [=]
        {
            EXIT_THREAD_DEBUG_INFO info = {};
            info.dwExitCode = ExitCode;
            setFakeDebugEvent(info);
            gCustomHandlers.at(UE_CH_EXITTHREAD)(&info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateProcess)(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp, ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset, ULONG64 StartOffset) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

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

                // NOTE: sanity check
                ULONG handleIndex = 0;
                hr = gDebugSystemObjects->GetProcessIdByHandle(Handle, &handleIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event process index by handle");
                    handleIndex = -1;
                }
                if (handleIndex != processIndex)
                    __debugbreak();
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

                // NOTE: sanity check
                ULONG handleIndex = 0;
                hr = gDebugSystemObjects->GetThreadIdByHandle(InitialThreadHandle, &handleIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event thread index by handle");
                    handleIndex = -1;
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
            gCustomHandlers.at(UE_CH_CREATEPROCESS)(&info);

            // TODO: CloseHandle(ImageFileHandle) if valid?

            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(ExitProcess)(ULONG ExitCode) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

        logDebug("[{}] Process exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);

        auto work = [=]
        {
            EXIT_PROCESS_DEBUG_INFO info = {};
            info.dwExitCode = ExitCode;
            setFakeDebugEvent(info);
            gCustomHandlers.at(UE_CH_EXITPROCESS)(&info);
            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(LoadModule)(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

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

            gCustomHandlers.at(UE_CH_LOADDLL)(&info);

            // TODO: CloseHandle(ImageFileHandle) if valid?

            return true;
        };
        queueCallback(std::move(work));

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(UnloadModule)(PCWSTR ImageBaseName, ULONG64 BaseOffset) override
    {
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

        logDebug("[{}] Module unloaded", __func__);
        logDebug("  ImageBaseName: {}", ImageBaseName ? Utf16ToUtf8(ImageBaseName) : "<unknown>");
        logDebug("  BaseOffset: {:#x}", BaseOffset);

        auto work = [=]
        {
            UNLOAD_DLL_DEBUG_INFO info = {};
            info.lpBaseOfDll = (LPVOID)BaseOffset;
            setFakeDebugEvent(info);
            gCustomHandlers.at(UE_CH_UNLOADDLL)(&info);
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
        if (GetCurrentThreadId() != mEventThreadId)
            __debugbreak();

        logDebug("[{}] Session status changed", __func__);
        logSingleFlag("Status", sessionFlags, Status);
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
        // We do not expect this to be called on the dbgeng thread, except during initialization
        if (GetCurrentThreadId() == mEventThreadId && gDbgEngInitialized)
            __debugbreak();

        logDebug("[{}] Engine state changed (tid: {}, apc: {}, debug: {})", __func__, GetCurrentThreadId(), mEventThreadId, gDebugThreadId);
        logBitFlag("Flags", cesFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);
        if (Flags & DEBUG_CES_EXECUTION_STATUS)
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

            // NOTE: ChangeEngineState is called multiple times for a state changes sometimes.
            // The first time is when the state is changed using the API, the second time
            // is when the debuggee actually enters the new state. We only want to see the
            // latter, which happens while WaitForEvent is blocking on the debug thread.
            // Calling StepInto on the debug thread also triggers ChangeEngineState, so
            // we exclude that by checking the paused flag.
            // TODO: likely this can be tracked using DEBUG_STATUS_INSIDE_WAIT too
            if (!IsDebugThread() || gPaused)
            {
                logDebug("  [ignored state change]");
                return S_OK;
            }

            switch (Argument & ~(DEBUG_STATUS_INSIDE_WAIT | DEBUG_STATUS_WAIT_TIMEOUT))
            {
            case DEBUG_STATUS_STEP_OVER:
            case DEBUG_STATUS_STEP_INTO:
            case DEBUG_STATUS_STEP_BRANCH:
            case DEBUG_STATUS_REVERSE_STEP_OVER:
            case DEBUG_STATUS_REVERSE_STEP_INTO:
            case DEBUG_STATUS_REVERSE_STEP_BRANCH:
            {
                ULONG threadIndex = 0;
                auto hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
                    break;
                }

                auto itr = gStepCallbacks.find(threadIndex);
                if (itr == gStepCallbacks.end())
                {
                    // NOTE: this always happens because the engine reports the state change twice
                    logDebug("No step callback for thread index {}", threadIndex);
                    break;
                }

                logDebug("Step callback for thread index {}", threadIndex);

                auto callback = itr->second;
                gStepCallbacks.erase(itr);

                auto work = [callback]
                {
                    gNextExecutionStatus = DEBUG_STATUS_GO;
                    callback();
                    return true;
                };
                queueCallback(std::move(work));
            }
            break;
            }
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
        { "rip", UE_CIP }, // TODO: 32-bit support
        { "rsp", UE_CSP }, // TODO: 32-bit support
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
        for (ULONG i = 0; i < Count; i++)
        {
            if (Changed[i])
            {
                logDebug("Writing register {} ({})", i, Names[i]);
            }
        }
        return true;
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
            __debugbreak();
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
            __debugbreak();
            return 0;
        case DEBUG_VALUE_FLOAT64:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT64", index, Names[index]);
            __debugbreak();
            return 0;
        case DEBUG_VALUE_FLOAT80:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT80", index, Names[index]);
            __debugbreak();
            return 0;
        case DEBUG_VALUE_FLOAT82:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT82", index, Names[index]);
            __debugbreak();
            return 0;
        case DEBUG_VALUE_FLOAT128:
            logError("register[{}]: {} has DEBUG_VALUE_FLOAT128", index, Names[index]);
            __debugbreak();
            return 0;
        case DEBUG_VALUE_VECTOR64:
            logError("register[{}]: {} has DEBUG_VALUE_VECTOR64", index, Names[index]);
            __debugbreak();
            return 0;
        case DEBUG_VALUE_VECTOR128:
            logError("register[{}]: {} has DEBUG_VALUE_VECTOR128", index, Names[index]);
            __debugbreak();
            return 0;
        default:
            break;
        }
        return 0;
    }
};

static RegisterCache gRegisterCache;

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

    // Create primary debug client interface
    hr = DebugCreate(__uuidof(IDebugClient9), reinterpret_cast<void**>(&gDebugClient));
    if (FAILED(hr))
    {
        logError("Failed to create IDebugClient9: {:#x}", (uint32_t)hr);
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

    // TODO: wait for initialization to complete/fail
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

        // TODO: cleanup

        // If the gDbgEngEvent is signaled, we should exit the thread
        return 0;
    };

    auto initEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gDbgEngEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gDbgEngThread = CreateThread(nullptr, 0, dbgEngThreadProc, initEvent, 0, nullptr);
    WaitForSingleObject(initEvent, INFINITE);
    CloseHandle(initEvent);

    if (!gDbgEngInitialized)
    {
        SetEvent(gDbgEngEvent);
        WaitForSingleObject(gDbgEngThread, INFINITE);
        CloseHandle(gDbgEngThread);
        CloseHandle(gDbgEngEvent);
        gDbgEngEvent = nullptr;
        gDbgEngThread = nullptr;
        logError("Failed to initialize dbgeng");
        return false;
    }

    return true;
}

// TitanEngine.Dumper.functions:
__declspec(dllexport) ULONG_PTR ConvertVAtoFileOffsetEx(ULONG_PTR FileMapVA, DWORD FileSize, ULONG_PTR ImageBase, ULONG_PTR AddressToConvert, bool AddressIsRVA, bool ReturnType)
{
    // TODO: implement properly
    return 0;
    __debugbreak();
    return {};
}

__declspec(dllexport) ULONG_PTR ConvertFileOffsetToVA(ULONG_PTR FileMapVA, ULONG_PTR AddressToConvert, bool ReturnType)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool MemoryReadSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
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
        logError("Unknown process handle in MemoryReadSafe: {:#x}", (uint64_t)(ULONG_PTR)hProcess);
        __debugbreak();
        return 0;
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
        // TODO: switch hProcess?
        __debugbreak();
    }

    hr = gDebugDataSpaces->ReadVirtual((ULONG64)(ULONG_PTR)lpBaseAddress, lpBuffer, nSize, (ULONG*)lpNumberOfBytesRead);
    if (FAILED(hr))
    {
        logError("ReadVirtual failed: {:#x}", (uint32_t)hr);
        return false;
    }
    return true;
}

__declspec(dllexport) bool MemoryWriteSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    __debugbreak();
    return {};
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
        __debugbreak();
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
        __debugbreak();
        return 0;
    }
    return itr->second;
}

__declspec(dllexport) bool HideDebugger(HANDLE hProcess, TitanHideLevel HideLevel)
{
    __debugbreak();
    return {};
}

// TitanEngine.Debugger.functions:
__declspec(dllexport) PROCESS_INFORMATION* InitDebugW(const wchar_t* szFileName, const wchar_t* szCommandLine, const wchar_t* szCurrentFolder)
{
    std::wstring commandLine = szFileName;
    if (szCommandLine)
    {
        commandLine += L" ";
        commandLine += szCommandLine;
    }
    DEBUG_CREATE_PROCESS_OPTIONS options = {};
    // auto hr = gDebugClient->CreateProcess2Wide(0, commandLine.data(), &options, sizeof(options), szCurrentFolder, nullptr);
    HRESULT hr = gDebugClient->CreateProcessWide(0, (PWSTR)szFileName, DEBUG_ONLY_THIS_PROCESS);
    if (FAILED(hr))
    {
        logError("CreateProcess2Wide failed: {:#x}", (uint32_t)hr);
        return nullptr;
    }

    gDebugThreadId = GetCurrentThreadId();

    return &gProcessInfo;
}

__declspec(dllexport) bool StopDebug()
{
    auto hr = gDebugClient->EndSession(DEBUG_END_ACTIVE_TERMINATE);
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
        if (info.type == DEBUG_BREAKPOINT_CODE && info.offset == bpxAddress)
            return true;
    }
    return false;
}

__declspec(dllexport) bool SetBPX(ULONG_PTR bpxAddress, DWORD bpxType /* TitanSoftwareBreakpointType */, TITANCBSOFTBP bpxCallBack)
{
    PauseLock pl;
    if (!pl)
    {
        logError("SetBPX failed to acquire pause lock");
        return false;
    }

    BreakpointInfo info;
    info.type = DEBUG_BREAKPOINT_CODE;
    info.callback = bpxCallBack;
    auto hr = gDebugControl->AddBreakpoint2(info.type, DEBUG_ANY_ID, &info.bp);
    if (FAILED(hr))
    {
        logError("Failed to add breakpoint: {:#x}", (uint32_t)hr);
        return false;
    }
    info.offset = bpxAddress;
    hr = info.bp->SetOffset(bpxAddress);
    if (FAILED(hr))
    {
        logError("Failed to set breakpoint offset: {:#x}", (uint32_t)hr);
        return false;
    }

    auto flags = DEBUG_BREAKPOINT_ENABLED;
    if (bpxType & UE_SINGLESHOOT)
        flags |= DEBUG_BREAKPOINT_ONE_SHOT;
    hr = info.bp->SetFlags(flags);
    if (FAILED(hr))
    {
        logError("Failed to enable breakpoint: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = info.bp->GetId(&info.id);
    if (FAILED(hr))
    {
        logError("Failed to get breakpoint ID: {:#x}", (uint32_t)hr);
        return false;
    }

    gBreakpoints.emplace(info.id, info);

    return true;
}

__declspec(dllexport) bool DeleteBPX(ULONG_PTR bpxAddress)
{
    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.type == DEBUG_BREAKPOINT_CODE && info.offset == bpxAddress)
        {
            auto hr = gDebugControl->RemoveBreakpoint2(info.bp);
            if (FAILED(hr))
            {
                logError("Failed to remove breakpoint: {:#x}", (uint32_t)hr);
                return false;
            }
            gBreakpoints.erase(id);
            return true;
        }
    }
    return false;
}

__declspec(dllexport) bool SetMemoryBPXEx(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory, TitanMemoryBreakpointType BreakPointType, bool RestoreOnHit, TITANCBMEMBP bpxCallBack)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool RemoveMemoryBPX(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool GetFullContextDataEx(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    // Zero out the context structure to avoid uninitialized data
    *titcontext = {};

    PauseLock pl;
    if (!pl)
    {
        return false;
    }

    auto itr = gDebugIdMap.threadHandleToIndex.find(hActiveThread);
    if (itr == gDebugIdMap.threadHandleToIndex.end())
    {
        logError("Unknown thread handle in GetFullContextDataEx: {:#x}", (uint64_t)(ULONG_PTR)hActiveThread);
        __debugbreak();
        return false;
    }
    auto threadIndex = itr->second;
    ULONG currentIndex = 0;
    auto hr = gDebugSystemObjects->GetCurrentThreadId(&currentIndex);
    if (FAILED(hr))
    {
        logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
        return false;
    }
    if (threadIndex != currentIndex)
    {
        // TODO: thread switching
        return false;
    }

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

    // TODO: support xmm/ymm registers

    return true;
}

__declspec(dllexport) bool SetFullContextDataEx(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) ULONG_PTR GetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister)
{
    PauseLock pl;
    if (!pl)
    {
        return 0;
    }

    auto itr = gDebugIdMap.threadHandleToIndex.find(hActiveThread);
    if (itr == gDebugIdMap.threadHandleToIndex.end())
    {
        logError("Unknown thread handle in GetContextDataEx: {:#x}", (uint64_t)(ULONG_PTR)hActiveThread);
        __debugbreak();
        return 0;
    }
    auto threadIndex = itr->second;
    ULONG currentIndex = 0;
    auto hr = gDebugSystemObjects->GetCurrentThreadId(&currentIndex);
    if (FAILED(hr))
    {
        logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
        return 0;
    }
    if (threadIndex != currentIndex)
    {
        // TODO: implement thread switching
        return 0;
    }
    return gRegisterCache.GetValue(IndexOfRegister);
}

__declspec(dllexport) bool SetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister, ULONG_PTR NewRegisterValue)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool GetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    PauseLock pl;
    if (!pl)
    {
        return false;
    }

    auto itr = gDebugIdMap.threadHandleToIndex.find(hActiveThread);
    if (itr == gDebugIdMap.threadHandleToIndex.end())
    {
        logError("Unknown thread handle in GetAVXContext: {:#x}", (uint64_t)(ULONG_PTR)hActiveThread);
        __debugbreak();
        return false;
    }
    auto threadIndex = itr->second;
    ULONG currentIndex = 0;
    auto hr = gDebugSystemObjects->GetCurrentThreadId(&currentIndex);
    if (FAILED(hr))
    {
        logError("Failed to get current thread index: {:#x}", (uint32_t)hr);
        return false;
    }
    if (threadIndex != currentIndex)
    {
        // TODO: implement thread switching
        return false;
    }

    // TODO: implement xmm/ymm registers

    return true;
}

__declspec(dllexport) bool SetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool GetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    // TODO: implement AVX-512 support
    // Fall back to using AVX and fill the rest with 0
    TITAN_ENGINE_CONTEXT_t Avx = {};
    if (GetAVXContext(hActiveThread, &Avx))
    {
        for (int i = 0; i < _countof(Avx.YmmRegisters); i++)
            titcontext->ZmmRegisters[i].Low = Avx.YmmRegisters[i];
        return true;
    }
    else
    {
        return false;
    }
}

__declspec(dllexport) bool SetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool Fill(LPVOID MemoryStart, DWORD MemorySize, PBYTE FillByte)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) const DEBUG_EVENT* GetDebugData()
{
    if (!IsDebugThread())
    {
        logError("GetDebugData called from non-debug thread!");
        return nullptr;
    }
    return &gFakeDebugEvent;
}

__declspec(dllexport) void SetCustomHandler(TitanCustomHandler ExceptionId, TITANCALLBACKARG CallBack)
{
    if (!IsDebugThread())
        __debugbreak();
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
    auto hr = gDebugSystemObjects->GetCurrentProcessId(&threadIndex);
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

    gStepCallbacks.emplace(threadIndex, callback);
}

__declspec(dllexport) void StepInto(TITANCBSTEP traceCallBack)
{
    debugStep(DEBUG_STATUS_STEP_INTO, traceCallBack);
}

__declspec(dllexport) void StepOver(TITANCBSTEP traceCallBack)
{
    debugStep(DEBUG_STATUS_STEP_OVER, traceCallBack);
}

__declspec(dllexport) bool GetUnusedHardwareBreakPointRegister(LPDWORD RegisterIndex)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool SetHardwareBreakPoint(ULONG_PTR bpxAddress, DWORD IndexOfRegister, TitanHardwareBreakpointType bpxType, TitanHardwareBreakpointSize bpxSize, TITANCBHWBP bpxCallBack)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool DeleteHardwareBreakPoint(DWORD IndexOfRegister)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool RemoveAllBreakPoints(TitanBreakpointRemoveOption RemoveOption)
{
    PauseLock pl;
    if (!pl)
    {
        logError("RemoveAllBreakPoints failed to acquire pause lock");
        return false;
    }

    for (const auto& [id, info] : gBreakpoints)
    {
        auto hr = gDebugControl->RemoveBreakpoint2(info.bp);
        if (FAILED(hr))
        {
            logError("Failed to remove breakpoint {}: {:#x}", id, (uint32_t)hr);
            return false;
        }
    }
    gBreakpoints.clear();

    return true;
}

__declspec(dllexport) void DebugLoop()
{
    if (!IsDebugThread())
        __debugbreak();
    logDebug("[{}] dwThreadId: {:#x}", __func__, GetCurrentThreadId());

    gIsDebugging = true;
    while (true)
    {
        // NOTE: The idea with gPaused and gMutexPaused is to allow other threads to read registers while we're paused
        // but while waiting for an event all those functions will fail instead.
        gPaused = false;
        ULONG execStatusBefore = 0;
        auto hr = gDebugControl->GetExecutionStatusEx(&execStatusBefore);
        if (FAILED(hr))
        {
            logError("[WaitForEvent] Failed to get execution status: {:#x}", (uint32_t)hr);
        }
        else
        {
            logDebug("[WaitForEvent] ExecutionStatus (before): {}", formatSingleFlag(statusFlags, execStatusBefore));
        }

        hr = gDebugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            logError("Failed to wait for initial event: {:#x}", (uint32_t)hr);
            break;
        }

        ULONG execStatusAfter = 0;
        hr = gDebugControl->GetExecutionStatusEx(&execStatusAfter);
        if (FAILED(hr))
        {
            logError("[WaitForEvent] Failed to get execution status: {:#x}", (uint32_t)hr);
        }
        else
        {
            logDebug("[WaitForEvent] ExecutionStatus (after): {}", formatSingleFlag(statusFlags, execStatusAfter));
        }

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

        std::lock_guard lgQueue(gMutexCallbackQueue);
        if (gCallbackQueue.empty())
        {
            __debugbreak();
        }
        // TODO: this should never be more than 1?
        logDebug("Processing {} queued callbacks", gCallbackQueue.size());
        while (!gCallbackQueue.empty())
        {
            auto work = std::move(gCallbackQueue.front());
            gCallbackQueue.pop();
            if (!work())
            {
                logError("worker callback failed");
                __debugbreak();
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
            hr = gDebugControl->SetExecutionStatus(gNextExecutionStatus);
            if (FAILED(hr))
            {
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
    gDebugIdMap = {};
}

__declspec(dllexport) void SetNextDbgContinueStatus(DWORD SetDbgCode)
{
    switch (SetDbgCode)
    {
    case DBG_EXCEPTION_NOT_HANDLED:
        logDebug("Next continue status: DBG_EXCEPTION_NOT_HANDLED");
        break;
    case DBG_CONTINUE:
        logDebug("Next continue status: DBG_CONTINUE");
        // TODO: how to continue?
        break;
    default:
        __debugbreak();
    }
}

__declspec(dllexport) bool AttachDebugger(DWORD ProcessId, bool KillOnExit, LPVOID DebugInfo, TITANCALLBACK CallBack)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool DetachDebuggerEx(DWORD ProcessId)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool IsFileBeingDebugged()
{
    return gIsDebugging;
}

// TitanEngine.Process.functions:
__declspec(dllexport) HANDLE TitanOpenProcess(DWORD dwDesiredAccess, bool bInheritHandle, DWORD dwProcessId)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) HANDLE TitanOpenThread(DWORD dwDesiredAccess, bool bInheritHandle, DWORD dwThreadId)
{
    __debugbreak();
    return {};
}

static std::map<ULONG_PTR, FileMap<uint8_t>*> gMappedFiles;

// TitanEngine.StaticUnpacker.functions:
__declspec(dllexport) bool StaticFileLoadW(const wchar_t* szFileName, DWORD DesiredAccess, bool SimulateLoad, LPHANDLE FileHandle, LPDWORD LoadedSize, LPHANDLE FileMap, PULONG_PTR FileMapVA)
{
    if (SimulateLoad)
        __debugbreak();

    auto file = new ::FileMap<uint8_t>;
    if (!file->Map(szFileName, DesiredAccess == UE_ACCESS_ALL))
        __debugbreak(); // return false;
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
        __debugbreak(); // return false;
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
__declspec(dllexport) void SetEngineVariable(TitanEngineVariable VariableId, bool VariableSet)
{
    gEngineVariables[VariableId] = VariableSet;
}

__declspec(dllexport) bool EngineCheckStructAlignment(TitanStructureType StructureType, ULONG_PTR StructureSize)
{
    // NOTE: This is called when x64dbg starts, so we can do initialization here
    if (!InitializeDbgEng())
        return false;

    if (StructureType == UE_STRUCT_TITAN_ENGINE_CONTEXT)
        return StructureSize == sizeof(TITAN_ENGINE_CONTEXT_t);
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
            gDbgEngThread = nullptr;
        }
    }
    return TRUE;
}
