#include <map>
#include <format>
#include <vector>

#include "TitanEngine.h"
#include "FileMap.h"

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

static decltype(&printf) _plugin_logprintf;

template<class... Args>
void logDebug(const std::format_string<Args...> fmt, Args&&... args)
{
    _plugin_logprintf("[dbgeng:debug] %s\n", std::format(fmt, std::forward<Args>(args)...).c_str());
}

template<class... Args>
void logError(const std::format_string<Args...> fmt, Args&&... args)
{
    _plugin_logprintf("[dbgeng:error] %s\n", std::format(fmt, std::forward<Args>(args)...).c_str());
}

template<class... Args>
void print(const std::format_string<Args...> fmt, Args&&... args)
{
    _plugin_logprintf("%s\n", std::format(fmt, std::forward<Args>(args)...).c_str());
}

std::string Utf16ToUtf8(const wchar_t* wstr)
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
    FLAG(DEBUG_STATUS_NO_CHANGE, ""),
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

static std::map<TitanEngineVariable, bool> gEngineVariables;
static TitanBreakpointType gDefaultBreakpointType = UE_BREAKPOINT_INT3;

// Event callback class
class DebugEventCallbacks : public IDebugEventCallbacksWide
{
private:
    ULONG mRefCount = 1;

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
        Bp->GetOffset(&offset);
        logDebug("  Offset: {:#x}", offset);
        ULONG breakType = 0;
        ULONG procType = 0;
        Bp->GetType(&breakType, &procType);
        logDebug("  BreakType: {:#x}", breakType);
        logDebug("  ProcType: {:#x}", procType);
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

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateThread)(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override
    {
        logDebug("[{}] Thread created", __func__);
        logDebug("  Handle: {:#x}", Handle);
        logDebug("  DataOffset: {:#x}", DataOffset);
        logDebug("  StartOffset: {:#x}", StartOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitThread)(ULONG ExitCode) override
    {
        logDebug("[{}] Thread exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(CreateProcess)(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp, ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset, ULONG64 StartOffset) override
    {
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
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitProcess)(ULONG ExitCode) override
    {
        logDebug("[{}] Process exited", __func__);
        logDebug("  ExitCode: {:#x}", ExitCode);
        return DEBUG_STATUS_NO_CHANGE;
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
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(UnloadModule)(PCWSTR ImageBaseName, ULONG64 BaseOffset) override
    {
        logDebug("[{}] Module unloaded", __func__);
        logDebug("  ImageBaseName: {}", ImageBaseName ? Utf16ToUtf8(ImageBaseName) : "<unknown>");
        logDebug("  BaseOffset: {:#x}", BaseOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SystemError)(ULONG Error, ULONG Level) override
    {
        logDebug("[{}] System error", __func__);
        logDebug("  Error: {:#x}", Error);
        logDebug("  Level: {:#x}", Level);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SessionStatus)(ULONG Status) override
    {
        logDebug("[{}] Session status changed", __func__);
        logSingleFlag("Status", sessionFlags, Status);
        return S_OK;
    }

    STDMETHOD(ChangeDebuggeeState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("[{}] Debuggee state changed", __func__);
        logSingleFlag("Flags", cdsFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ChangeEngineState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("[{}] Engine state changed", __func__);
        logBitFlag("Flags", cesFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);
        if (Flags & DEBUG_CES_EXECUTION_STATUS)
        {
            if (Argument & DEBUG_STATUS_INSIDE_WAIT)
            {
                logDebug("  DEBUG_STATUS_INSIDE_WAIT");
                Argument &= ~DEBUG_STATUS_INSIDE_WAIT;
            }
            if (Argument & DEBUG_STATUS_WAIT_TIMEOUT)
            {
                logDebug("  DEBUG_STATUS_WAIT_TIMEOUT");
                Argument &= ~DEBUG_STATUS_WAIT_TIMEOUT;
            }
            logSingleFlag("Status", statusFlags, Argument);
        }
        return S_OK;
    }

    STDMETHOD(ChangeSymbolState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("[{}] Symbol state changed", __func__);
        logBitFlag("Flags", cssFlags, Flags);
        logDebug("  Argument: {:#x}", Argument);

        return DEBUG_STATUS_NO_CHANGE;
    }
};

static IDebugClient9* gDebugClient = nullptr;
static IDebugControl* gDebugControl = nullptr;
static IDebugDataSpaces* gDebugDataSpaces = nullptr;
static IDebugRegisters* gDebugRegisters = nullptr;
static IDebugSymbols* gDebugSymbols = nullptr;
static IDebugSystemObjects* gDebugSystemObjects = nullptr;

static DebugEventCallbacks* gEventCallbacks = nullptr;
static std::vector<IDebugBreakpoint*> gBreakpoints;

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static bool InitializeDbgEng()
{
    static bool initialized = false;
    if (initialized)
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

    // Create primary debug client interface
    HRESULT hr = DebugCreate(__uuidof(IDebugClient9), reinterpret_cast<void**>(&gDebugClient));
    if (FAILED(hr))
    {
        logError("Failed to create IDebugClient9: {:#x}", (uint32_t)hr);
        return false;
    }

    // Query for other interfaces
    hr = gDebugClient->QueryInterface(__uuidof(IDebugControl), reinterpret_cast<void**>(&gDebugControl));
    if (FAILED(hr))
    {
        logError("Failed to get IDebugControl: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = gDebugClient->QueryInterface(__uuidof(IDebugDataSpaces), reinterpret_cast<void**>(&gDebugDataSpaces));
    if (FAILED(hr))
    {
        logError("Failed to get IDebugDataSpaces: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = gDebugClient->QueryInterface(__uuidof(IDebugRegisters), reinterpret_cast<void**>(&gDebugRegisters));
    if (FAILED(hr))
    {
        logError("Failed to get IDebugRegisters: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = gDebugClient->QueryInterface(__uuidof(IDebugSymbols), reinterpret_cast<void**>(&gDebugSymbols));
    if (FAILED(hr))
    {
        logError("Failed to get IDebugSymbols: {:#x}", (uint32_t)hr);
        return false;
    }

    hr = gDebugClient->QueryInterface(__uuidof(IDebugSystemObjects), reinterpret_cast<void**>(&gDebugSystemObjects));
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

    initialized = true;
    return true;
}

// TitanEngine.Dumper.functions:
__declspec(dllexport) ULONG_PTR ConvertVAtoFileOffsetEx(ULONG_PTR FileMapVA, DWORD FileSize, ULONG_PTR ImageBase, ULONG_PTR AddressToConvert, bool AddressIsRVA, bool ReturnType)
{
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
    __debugbreak();
    return {};
}

__declspec(dllexport) bool MemoryWriteSafe(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    __debugbreak();
    return {};
}

// TitanEngine.Hider.functions:
__declspec(dllexport) void* GetPEBLocation(HANDLE hProcess)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) void* GetTEBLocation(HANDLE hThread)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool HideDebugger(HANDLE hProcess, TitanHideLevel HideLevel)
{
    __debugbreak();
    return {};
}

// TitanEngine.Debugger.functions:
__declspec(dllexport) PROCESS_INFORMATION* InitDebugW(const wchar_t* szFileName, const wchar_t* szCommandLine, const wchar_t* szCurrentFolder)
{
    static PROCESS_INFORMATION pi = {};
    logError("InitDebugW not implemented!");
    return nullptr;
    return &pi;
}

__declspec(dllexport) bool StopDebug()
{
    __debugbreak();
    return {};
}

__declspec(dllexport) void SetBPXOptions(TitanBreakpointType DefaultBreakPointType)
{
    gDefaultBreakpointType = DefaultBreakPointType;
}

__declspec(dllexport) bool IsBPXEnabled(ULONG_PTR bpxAddress)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool SetBPX(ULONG_PTR bpxAddress, DWORD bpxType, TITANCBSOFTBP bpxCallBack)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool DeleteBPX(ULONG_PTR bpxAddress)
{
    __debugbreak();
    return {};
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
    __debugbreak();
    return {};
}

__declspec(dllexport) bool SetFullContextDataEx(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) ULONG_PTR GetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool SetContextDataEx(HANDLE hActiveThread, TitanRegister IndexOfRegister, ULONG_PTR NewRegisterValue)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool GetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool SetAVXContext(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_t* titcontext)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool GetAVX512Context(HANDLE hActiveThread, TITAN_ENGINE_CONTEXT_AVX512_t* titcontext)
{
    __debugbreak();
    return {};
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
    __debugbreak();
    return {};
}

__declspec(dllexport) void SetCustomHandler(TitanCustomHandler ExceptionId, TITANCBCH CallBack)
{
    __debugbreak();
}

__declspec(dllexport) void StepInto(TITANCBSTEP traceCallBack)
{
    __debugbreak();
}

__declspec(dllexport) void StepOver(TITANCBSTEP traceCallBack)
{
    __debugbreak();
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
    __debugbreak();
    return {};
}

__declspec(dllexport) void DebugLoop()
{
    __debugbreak();
}

__declspec(dllexport) void SetNextDbgContinueStatus(DWORD SetDbgCode)
{
    __debugbreak();
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
    __debugbreak();
    return {};
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
