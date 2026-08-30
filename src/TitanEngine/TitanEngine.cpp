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
    bool nativePatch = false;
    BYTE originalByte = 0;
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
static ULONG gNextNativeBreakpointId = 0x80000000;
static std::set<ULONG64> gRetiredBreakpointAddresses;
static std::recursive_mutex gMutexPaused;
static std::atomic_bool gPaused; // this is set to true when we are not inside WaitForEvent (perhaps it should be renamed?)
static std::atomic_bool gIsDebugging;
static bool gDbgEngInitialized = false;
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
        if (!gStepStatuses.empty())
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

        auto work = [=]
        {
            // TODO: support other breakpoint types
            EXCEPTION_DEBUG_INFO info = {};
            info.ExceptionRecord.ExceptionCode = EXCEPTION_BREAKPOINT;
            info.ExceptionRecord.ExceptionFlags = 0;
            info.ExceptionRecord.ExceptionAddress = (PVOID)offset;
            setFakeDebugEvent(info);

            auto itr = gBreakpoints.find(id);
            if (itr == gBreakpoints.end())
            {
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

            beginDebugEvent(false);
            const auto& bpInfo = itr->second;
            if (bpInfo.callback)
                bpInfo.callback();
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
            if (info.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && gExpectSystemBreakpoint)
            {
                gExpectSystemBreakpoint = false;
                dispatchSystemBreakpoint(&info);
            }
            else if (info.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
            {
                const auto address = (ULONG64)(ULONG_PTR)info.ExceptionRecord.ExceptionAddress;
                auto nativeBreakpoint = std::find_if(gBreakpoints.begin(), gBreakpoints.end(), [address](const auto& entry)
                {
                    return entry.second.nativePatch && entry.second.offset == address;
                });
                if (nativeBreakpoint != gBreakpoints.end())
                {
                    setCurrentInstructionPointer((ULONG_PTR)address);
                    beginDebugEvent(false);
                    if (nativeBreakpoint->second.callback)
                        nativeBreakpoint->second.callback();
                    finishDebugEvent(false);
                }
                else if (gRetiredBreakpointAddresses.contains(address))
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
            dispatchDebugEvent(UE_CH_EXITTHREAD, &info);
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

                // NOTE: sanity check
                ULONG handleIndex = 0;
                hr = gDebugSystemObjects->GetProcessIdByHandle(Handle, &handleIndex);
                if (FAILED(hr))
                {
                    logError("Failed to get event process index by handle");
                    handleIndex = -1;
                }
                if (handleIndex != processIndex)
                    logError("Process index mismatch: current {}, handle {}", processIndex, handleIndex);
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
            dispatchDebugEvent(UE_CH_CREATEPROCESS, &info);

            // TODO: CloseHandle(ImageFileHandle) if valid?

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
            dispatchDebugEvent(UE_CH_EXITPROCESS, &info);
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

            // TODO: CloseHandle(ImageFileHandle) if valid?

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

static void setCurrentInstructionPointer(ULONG_PTR address)
{
    gRegisterCache.SetValue(UE_RIP, address);
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

__declspec(dllexport) bool MemoryReadSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
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
        logError("MemoryReadSafe does not support a non-current process yet");
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }

    ULONG bytesRead = 0;
    hr = gDebugDataSpaces->ReadVirtual((ULONG64)(ULONG_PTR)lpBaseAddress, lpBuffer, (ULONG)std::min<SIZE_T>(nSize, ULONG_MAX), &bytesRead);
    if (lpNumberOfBytesRead)
        *lpNumberOfBytesRead = bytesRead;
    if (FAILED(hr))
    {
        logError("ReadVirtual failed: {:#x}", (uint32_t)hr);
        return false;
    }
    return bytesRead == nSize;
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
    return VirtualQueryEx(hProcess, lpAddress, lpBuffer, dwLength);
}

__declspec(dllexport) LPVOID MemoryAllocSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    return VirtualAllocEx(hProcess, lpAddress, dwSize, flAllocationType, flProtect);
}

__declspec(dllexport) bool MemoryFreeSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    return !!VirtualFreeEx(hProcess, lpAddress, dwSize, dwFreeType);
}

__declspec(dllexport) bool MemoryProtectSafe(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect)
{
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

__declspec(dllexport) bool HideDebugger(HANDLE hProcess, DWORD HideLevel)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
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
    ResetEvent(gProcessCreatedEvent);
    gDebugThreadId = GetCurrentThreadId();
    gExpectSystemBreakpoint = true;
    gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
    gNextContinueStatus = DBG_CONTINUE;

    DEBUG_CREATE_PROCESS_OPTIONS options = {};
    options.CreateFlags = DEBUG_ONLY_THIS_PROCESS;
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

    gProcessInfo.hProcess = initProcess;
    gProcessInfo.hThread = initThread;
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
    if (!pl && !gIsDebugging)
    {
        logError("SetBPX called without an active debug session");
        return false;
    }

    BreakpointInfo info;
    info.type = DEBUG_BREAKPOINT_CODE;
    info.callback = bpxCallBack;
    info.offset = bpxAddress;
    auto hr = gDebugControl->AddBreakpoint2(info.type, DEBUG_ANY_ID, &info.bp);
    if (FAILED(hr))
    {
        // DbgEng cannot update its breakpoint table while WaitForEvent is
        // running. The pause command suspends the selected native thread, so
        // use a temporary native INT3 and translate its exception below.
        if (!pl && gProcessInfo.hProcess)
        {
            SIZE_T transferred = 0;
            if (ReadProcessMemory(gProcessInfo.hProcess, (LPCVOID)bpxAddress, &info.originalByte, 1, &transferred) && transferred == 1)
            {
                const BYTE int3 = 0xCC;
                if (WriteProcessMemory(gProcessInfo.hProcess, (LPVOID)bpxAddress, &int3, 1, &transferred) && transferred == 1)
                {
                    FlushInstructionCache(gProcessInfo.hProcess, (LPCVOID)bpxAddress, 1);
                    info.nativePatch = true;
                    info.id = gNextNativeBreakpointId++;
                    gRetiredBreakpointAddresses.erase(bpxAddress);
                    gBreakpoints.emplace(info.id, info);
                    return true;
                }
            }
        }
        logError("Failed to add breakpoint: {:#x}", (uint32_t)hr);
        return false;
    }
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

    gRetiredBreakpointAddresses.erase(bpxAddress);
    gBreakpoints.emplace(info.id, info);

    return true;
}

__declspec(dllexport) bool DeleteBPX(ULONG_PTR bpxAddress)
{
    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.type == DEBUG_BREAKPOINT_CODE && info.offset == bpxAddress)
        {
            if (info.nativePatch)
            {
                SIZE_T written = 0;
                if (!WriteProcessMemory(gProcessInfo.hProcess, (LPVOID)bpxAddress, &info.originalByte, 1, &written) || written != 1)
                    return false;
                FlushInstructionCache(gProcessInfo.hProcess, (LPCVOID)bpxAddress, 1);
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
    return false;
}

__declspec(dllexport) bool SetMemoryBPXEx(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory, TitanMemoryBreakpointType BreakPointType, bool RestoreOnHit, TITANCBMEMBP bpxCallBack)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool RemoveMemoryBPX(ULONG_PTR MemoryStart, SIZE_T SizeOfMemory)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
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

    const std::pair<TitanRegister, ULONG_PTR> values[] = {
        { UE_RAX, titcontext->cax }, { UE_RBX, titcontext->cbx }, { UE_RCX, titcontext->ccx },
        { UE_RDX, titcontext->cdx }, { UE_RSI, titcontext->csi }, { UE_RDI, titcontext->cdi },
        { UE_RBP, titcontext->cbp }, { UE_RSP, titcontext->csp }, { UE_RIP, titcontext->cip },
        { UE_RFLAGS, titcontext->eflags }, { UE_R8, titcontext->r8 }, { UE_R9, titcontext->r9 },
        { UE_R10, titcontext->r10 }, { UE_R11, titcontext->r11 }, { UE_R12, titcontext->r12 },
        { UE_R13, titcontext->r13 }, { UE_R14, titcontext->r14 }, { UE_R15, titcontext->r15 },
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
    return gRegisterCache.Flush();
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
    PauseLock pl;
    if (!pl)
    {
        return false;
    }

    DebugThreadScope threadScope(hActiveThread);
    if (!threadScope)
        return false;

    // General registers are supplied by GetFullContextDataEx. Vector register
    // transfer is not implemented yet, so report this optional portion as
    // unsupported without interrupting the debug session.
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool SetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool GetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    if (!titcontext)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    *titcontext = {};
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
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool Fill(LPVOID MemoryStart, DWORD MemorySize, PBYTE FillByte)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
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

    gStepCallbacks.emplace(threadIndex, callback);
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

__declspec(dllexport) bool GetUnusedHardwareBreakPointRegister(LPDWORD RegisterIndex)
{
    if (RegisterIndex)
        *RegisterIndex = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool SetHardwareBreakPoint(ULONG_PTR bpxAddress, DWORD IndexOfRegister, TitanHardwareBreakpointType bpxType, TitanHardwareBreakpointSize bpxSize, TITANCBHWBP bpxCallBack)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool DeleteHardwareBreakPoint(DWORD IndexOfRegister)
{
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

__declspec(dllexport) bool RemoveAllBreakPoints(TitanBreakpointRemoveOption RemoveOption)
{
    PauseLock pl;
    if (!pl)
    {
        if (!gIsDebugging)
        {
            gBreakpoints.clear();
            gRetiredBreakpointAddresses.clear();
            return true;
        }
        logError("RemoveAllBreakPoints failed to acquire pause lock");
        return false;
    }

    for (const auto& [id, info] : gBreakpoints)
    {
        if (info.nativePatch)
        {
            SIZE_T written = 0;
            if (!WriteProcessMemory(gProcessInfo.hProcess, (LPVOID)info.offset, &info.originalByte, 1, &written) || written != 1)
                return false;
            FlushInstructionCache(gProcessInfo.hProcess, (LPCVOID)info.offset, 1);
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
                logError("Failed to wait for event: {:#x}", (uint32_t)hr);
                break;
            }

            bool hasEventCallback = false;
            {
                std::lock_guard lgQueue(gMutexCallbackQueue);
                hasEventCallback = !gCallbackQueue.empty();
            }
            if (!hasEventCallback && !gStepCallbacks.empty())
            {
                ULONG threadIndex = 0;
                hr = gDebugSystemObjects->GetCurrentThreadId(&threadIndex);
                auto step = gStepCallbacks.find(threadIndex);
                if (SUCCEEDED(hr) && step != gStepCallbacks.end())
                {
                    logDebug("Step completed for thread index {}", threadIndex);
                    const auto callback = step->second;
                    gStepCallbacks.erase(step);
                    gStepStatuses.erase(threadIndex);
                    queueCallback([callback]
                    {
                        gNextExecutionStatus = DEBUG_STATUS_GO;
                        callback();
                        return true;
                    });
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
    gBreakpoints = {};
    gStepCallbacks = {};
    gStepStatuses = {};
    gRetiredBreakpointAddresses = {};
    gAttachCallback = nullptr;
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
    ResetEvent(gProcessCreatedEvent);
    gDebugThreadId = GetCurrentThreadId();
    gExpectSystemBreakpoint = true;
    gAttachCallback = CallBack;
    gNextExecutionStatus = DEBUG_STATUS_NO_CHANGE;
    gNextContinueStatus = DBG_CONTINUE;

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

    *static_cast<PROCESS_INFORMATION*>(DebugInfo) = gProcessInfo;
    DebugLoop();
    return true;
}

__declspec(dllexport) bool DetachDebuggerEx(DWORD ProcessId)
{
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
    return OpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
}

__declspec(dllexport) HANDLE TitanOpenThread(DWORD dwDesiredAccess, bool bInheritHandle, DWORD dwThreadId)
{
    return OpenThread(dwDesiredAccess, bInheritHandle, dwThreadId);
}

__declspec(dllexport) bool TitanCloseHandle(HANDLE hEngineHandle)
{
    return hEngineHandle && !!CloseHandle(hEngineHandle);
}

__declspec(dllexport) bool ProcessIsWow64(HANDLE hProcess, PBOOL isWow64)
{
    if (!hProcess || !isWow64)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    return !!IsWow64Process(hProcess, isWow64);
}

__declspec(dllexport) bool TitanTerminateProcess(HANDLE hProcess, DWORD exitCode) { return !!TerminateProcess(hProcess, exitCode); }
__declspec(dllexport) bool TitanDebugBreakProcess(HANDLE hProcess) { return !!DebugBreakProcess(hProcess); }
__declspec(dllexport) HANDLE TitanCreateRemoteThread(HANDLE hProcess, LPTHREAD_START_ROUTINE start, LPVOID argument, DWORD creationFlags, LPDWORD threadId) { return CreateRemoteThread(hProcess, nullptr, 0, start, argument, creationFlags, threadId); }
__declspec(dllexport) DWORD TitanSuspendThread(HANDLE hThread) { return SuspendThread(hThread); }
__declspec(dllexport) DWORD TitanResumeThread(HANDLE hThread) { return ResumeThread(hThread); }
__declspec(dllexport) bool TitanTerminateThread(HANDLE hThread, DWORD exitCode) { return !!TerminateThread(hThread, exitCode); }
__declspec(dllexport) DWORD TitanGetThreadId(HANDLE hThread) { return GetThreadId(hThread); }
__declspec(dllexport) int TitanGetThreadPriority(HANDLE hThread) { return GetThreadPriority(hThread); }
__declspec(dllexport) bool TitanSetThreadPriority(HANDLE hThread, int priority) { return !!SetThreadPriority(hThread, priority); }
__declspec(dllexport) bool TitanGetThreadTimes(HANDLE hThread, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user) { return !!GetThreadTimes(hThread, creation, exit, kernel, user); }
__declspec(dllexport) bool TitanQueryThreadCycleTime(HANDLE hThread, PULONG64 cycleTime) { return !!QueryThreadCycleTime(hThread, cycleTime); }

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
            CloseHandle(gDbgEngEvent);
            CloseHandle(gProcessCreatedEvent);
            gDbgEngThread = nullptr;
            gDbgEngEvent = nullptr;
            gProcessCreatedEvent = nullptr;
        }
    }
    return TRUE;
}
