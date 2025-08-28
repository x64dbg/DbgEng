#include <map>

#include "TitanEngine.h"
#include "FileMap.h"

static std::map<TitanEngineVariable, bool> mEngineVariables;
static TitanBreakpointType mDefaultBreakpointType = UE_BREAKPOINT_INT3;

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
__declspec(dllexport) void* InitDebugW(const wchar_t* szFileName, const wchar_t* szCommandLine, const wchar_t* szCurrentFolder)
{
    __debugbreak();
    return {};
}

__declspec(dllexport) bool StopDebug()
{
    __debugbreak();
    return {};
}

__declspec(dllexport) void SetBPXOptions(TitanBreakpointType DefaultBreakPointType)
{
    mDefaultBreakpointType = DefaultBreakPointType;
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
    mEngineVariables[VariableId] = VariableSet;
}

__declspec(dllexport) bool EngineCheckStructAlignment(TitanStructureType StructureType, ULONG_PTR StructureSize)
{
    if (StructureType == UE_STRUCT_TITAN_ENGINE_CONTEXT)
        return StructureSize == sizeof(TITAN_ENGINE_CONTEXT_t);
    return false;
}
