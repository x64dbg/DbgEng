#include <Windows.h>
#include <DbgEng.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{

void printHr(const char* operation, HRESULT hr)
{
    std::printf("%-36s hr=0x%08lx%s\n", operation, static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "" : " FAILED");
}

template<typename T>
void release(T*& value)
{
    if(value)
    {
        value->Release();
        value = nullptr;
    }
}

class OutputCallbacks final : public IDebugOutputCallbacksWide
{
public:
    STDMETHOD(QueryInterface)(REFIID iid, void** object) override
    {
        if(!object)
            return E_INVALIDARG;
        *object = nullptr;
        if(iid != __uuidof(IUnknown) && iid != __uuidof(IDebugOutputCallbacksWide))
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }

    STDMETHOD_(ULONG, AddRef)() override
    {
        return InterlockedIncrement(&mRefCount);
    }

    STDMETHOD_(ULONG, Release)() override
    {
        const auto count = InterlockedDecrement(&mRefCount);
        if(!count)
            delete this;
        return count;
    }

    STDMETHOD(Output)(ULONG mask, PCWSTR text) override
    {
        std::wprintf(L"[dbgeng-output:0x%08lx] %ls", static_cast<unsigned long>(mask), text ? text : L"");
        return S_OK;
    }

private:
    LONG mRefCount = 1;
};

const char* debugClassName(ULONG value)
{
    switch(value)
    {
    case DEBUG_CLASS_UNINITIALIZED: return "uninitialized";
    case DEBUG_CLASS_KERNEL: return "kernel";
    case DEBUG_CLASS_USER_WINDOWS: return "user-windows";
    case DEBUG_CLASS_IMAGE_FILE: return "image-file";
    default: return "unknown";
    }
}

const char* qualifierName(ULONG value)
{
    switch(value)
    {
    case DEBUG_USER_WINDOWS_PROCESS: return "user-process";
    case DEBUG_USER_WINDOWS_PROCESS_SERVER: return "user-process-server";
    case DEBUG_USER_WINDOWS_IDNA: return "user-idna";
    case DEBUG_USER_WINDOWS_REPT: return "user-rept";
    case DEBUG_DUMP_SMALL: return "small-dump";
    case DEBUG_DUMP_DEFAULT: return "dump";
    case DEBUG_DUMP_FULL: return "full-dump";
    case DEBUG_DUMP_IMAGE_FILE: return "image-file";
    case DEBUG_DUMP_TRACE_LOG: return "trace-log";
    case DEBUG_DUMP_WINDOWS_CE: return "windows-ce-dump";
    case DEBUG_DUMP_ACTIVE: return "active-dump";
    default: return "unknown";
    }
}

const wchar_t* movementName(ULONG status)
{
    switch(status)
    {
    case DEBUG_STATUS_GO: return L"forward-go";
    case DEBUG_STATUS_STEP_INTO: return L"forward-step-into";
    case DEBUG_STATUS_STEP_OVER: return L"forward-step-over";
    case DEBUG_STATUS_REVERSE_GO: return L"reverse-go";
    case DEBUG_STATUS_REVERSE_STEP_INTO: return L"reverse-step-into";
    case DEBUG_STATUS_REVERSE_STEP_OVER: return L"reverse-step-over";
    default: return L"unknown";
    }
}

void printLoadedRuntimeModules()
{
    constexpr std::array names = {
        L"dbgeng.dll", L"dbghelp.dll", L"dbgcore.dll", L"dbgmodel.dll",
        L"TTDReplay.dll", L"TTDReplayCPU.dll"
    };
    std::puts("runtime modules:");
    for(const auto* name : names)
    {
        const auto module = GetModuleHandleW(name);
        if(!module)
        {
            std::wprintf(L"  %-18ls <not loaded>\n", name);
            continue;
        }
        std::array<wchar_t, 32768> path = {};
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        std::wprintf(L"  %-18ls %ls\n", name, path.data());
    }
}

void printLastEvent(IDebugControl7* control)
{
    ULONG type = 0, process = 0, thread = 0, extraUsed = 0, descriptionUsed = 0;
    std::array<std::uint8_t, 1024> extra = {};
    std::array<wchar_t, 2048> description = {};
    const auto hr = control->GetLastEventInformationWide(
        &type, &process, &thread, extra.data(), static_cast<ULONG>(extra.size()), &extraUsed,
        description.data(), static_cast<ULONG>(description.size()), &descriptionUsed);
    printHr("GetLastEventInformationWide", hr);
    if(SUCCEEDED(hr))
    {
        std::printf("  type=%lu process-index=%lu thread-index=%lu extra-bytes=%lu\n",
                    static_cast<unsigned long>(type), static_cast<unsigned long>(process),
                    static_cast<unsigned long>(thread), static_cast<unsigned long>(extraUsed));
        std::wprintf(L"  description=%ls\n", description.data());
    }
}

void printThreads(IDebugSystemObjects4* systemObjects)
{
    ULONG count = 0;
    auto hr = systemObjects->GetNumberThreads(&count);
    printHr("GetNumberThreads", hr);
    if(FAILED(hr))
        return;
    std::printf("  count=%lu\n", static_cast<unsigned long>(count));
    std::vector<ULONG> engineIds(count), systemIds(count);
    hr = systemObjects->GetThreadIdsByIndex(0, count, engineIds.data(), systemIds.data());
    printHr("GetThreadIdsByIndex", hr);
    if(FAILED(hr))
        return;
    for(ULONG i = 0; i < count; ++i)
        std::printf("  [%lu] engine=%lu system=%lu\n", static_cast<unsigned long>(i),
                    static_cast<unsigned long>(engineIds[i]), static_cast<unsigned long>(systemIds[i]));
}

void printModules(IDebugSymbols5* symbols)
{
    ULONG loaded = 0, unloaded = 0;
    auto hr = symbols->GetNumberModules(&loaded, &unloaded);
    printHr("GetNumberModules", hr);
    if(FAILED(hr))
        return;
    std::printf("  loaded=%lu unloaded=%lu\n", static_cast<unsigned long>(loaded), static_cast<unsigned long>(unloaded));
    for(ULONG index = 0; index < loaded && index < 256; ++index)
    {
        ULONG64 base = 0;
        if(FAILED(symbols->GetModuleByIndex(index, &base)))
            continue;
        std::array<wchar_t, 32768> image = {};
        ULONG imageUsed = 0;
        const auto nameHr = symbols->GetModuleNameStringWide(
            DEBUG_MODNAME_IMAGE, index, base, image.data(), static_cast<ULONG>(image.size()), &imageUsed);
        std::printf("  [%lu] base=0x%016llx name-hr=0x%08lx ", static_cast<unsigned long>(index),
                    static_cast<unsigned long long>(base), static_cast<unsigned long>(nameHr));
        if(SUCCEEDED(nameHr))
            std::wprintf(L"%ls", image.data());
        std::putchar('\n');
    }
}

HRESULT exerciseMovement(IDebugControl7* control, IDebugRegisters2* registers, ULONG status)
{
    std::wprintf(L"movement: %ls (%lu)\n", movementName(status), static_cast<unsigned long>(status));
    auto hr = control->SetExecutionStatus(status);
    printHr("  SetExecutionStatus", hr);
    if(FAILED(hr))
        return hr;
    hr = control->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
    printHr("  WaitForEvent", hr);
    ULONG resultingStatus = 0;
    const auto statusHr = control->GetExecutionStatus(&resultingStatus);
    printHr("  GetExecutionStatus", statusHr);
    if(SUCCEEDED(statusHr))
        std::printf("  resulting-status=%lu\n", static_cast<unsigned long>(resultingStatus));
    ULONG64 instruction = 0;
    const auto instructionHr = registers->GetInstructionOffset(&instruction);
    printHr("  GetInstructionOffset", instructionHr);
    if(SUCCEEDED(instructionHr))
        std::printf("  instruction=0x%016llx\n", static_cast<unsigned long long>(instruction));
    return hr;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if(argc < 2)
    {
        std::fwprintf(stderr, L"usage: replay_probe <dump-or-trace> [--forward-step|--reverse-step|--forward-go|--reverse-go]\n");
        return 2;
    }

    auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(hr);
    if(FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        printHr("CoInitializeEx", hr);
        return 1;
    }

    IDebugClient5* client = nullptr;
    IDebugControl7* control = nullptr;
    IDebugSystemObjects4* systemObjects = nullptr;
    IDebugSymbols5* symbols = nullptr;
    IDebugDataSpaces4* dataSpaces = nullptr;
    IDebugRegisters2* registers = nullptr;
    OutputCallbacks* output = nullptr;

    hr = [&]() -> HRESULT
    {
        auto operationHr = DebugCreate(__uuidof(IDebugClient5), reinterpret_cast<void**>(&client));
        printHr("DebugCreate(IDebugClient5)", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        operationHr = client->QueryInterface(__uuidof(IDebugControl7), reinterpret_cast<void**>(&control));
        printHr("QueryInterface(IDebugControl7)", operationHr);
        if(FAILED(operationHr))
            return operationHr;
        operationHr = client->QueryInterface(__uuidof(IDebugSystemObjects4), reinterpret_cast<void**>(&systemObjects));
        printHr("QueryInterface(IDebugSystemObjects4)", operationHr);
        if(FAILED(operationHr))
            return operationHr;
        operationHr = client->QueryInterface(__uuidof(IDebugSymbols5), reinterpret_cast<void**>(&symbols));
        printHr("QueryInterface(IDebugSymbols5)", operationHr);
        if(FAILED(operationHr))
            return operationHr;
        operationHr = client->QueryInterface(__uuidof(IDebugDataSpaces4), reinterpret_cast<void**>(&dataSpaces));
        printHr("QueryInterface(IDebugDataSpaces4)", operationHr);
        if(FAILED(operationHr))
            return operationHr;
        operationHr = client->QueryInterface(__uuidof(IDebugRegisters2), reinterpret_cast<void**>(&registers));
        printHr("QueryInterface(IDebugRegisters2)", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        output = new OutputCallbacks();
        operationHr = client->SetOutputCallbacksWide(output);
        printHr("SetOutputCallbacksWide", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        operationHr = control->SetEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK | DEBUG_ENGOPT_DISABLE_MODULE_SYMBOL_LOAD);
        printHr("SetEngineOptions", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        std::wprintf(L"artifact: %ls\n", argv[1]);
        operationHr = client->OpenDumpFileWide(argv[1], 0);
        printHr("OpenDumpFileWide", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        operationHr = control->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        printHr("WaitForEvent(initial)", operationHr);
        if(FAILED(operationHr))
            return operationHr;

        ULONG debugClass = 0, qualifier = 0;
        operationHr = control->GetDebuggeeType(&debugClass, &qualifier);
        printHr("GetDebuggeeType", operationHr);
        if(SUCCEEDED(operationHr))
            std::printf("  class=%lu (%s) qualifier=%lu (%s)\n",
                        static_cast<unsigned long>(debugClass), debugClassName(debugClass),
                        static_cast<unsigned long>(qualifier), qualifierName(qualifier));

        ULONG actualMachine = 0, effectiveMachine = 0;
        printHr("GetActualProcessorType", control->GetActualProcessorType(&actualMachine));
        printHr("GetEffectiveProcessorType", control->GetEffectiveProcessorType(&effectiveMachine));
        std::printf("  actual-machine=0x%04lx effective-machine=0x%04lx\n",
                    static_cast<unsigned long>(actualMachine), static_cast<unsigned long>(effectiveMachine));

        ULONG processSystemId = 0, threadSystemId = 0;
        printHr("GetCurrentProcessSystemId", systemObjects->GetCurrentProcessSystemId(&processSystemId));
        printHr("GetCurrentThreadSystemId", systemObjects->GetCurrentThreadSystemId(&threadSystemId));
        std::printf("  process-system-id=%lu thread-system-id=%lu\n",
                    static_cast<unsigned long>(processSystemId), static_cast<unsigned long>(threadSystemId));

        ULONG64 peb = 0, teb = 0, instruction = 0;
        printHr("GetCurrentProcessPeb", systemObjects->GetCurrentProcessPeb(&peb));
        printHr("GetCurrentThreadTeb", systemObjects->GetCurrentThreadTeb(&teb));
        printHr("GetInstructionOffset", registers->GetInstructionOffset(&instruction));
        std::printf("  peb=0x%016llx teb=0x%016llx instruction=0x%016llx\n",
                    static_cast<unsigned long long>(peb), static_cast<unsigned long long>(teb),
                    static_cast<unsigned long long>(instruction));

        std::array<unsigned char, 16> bytes = {};
        ULONG bytesRead = 0;
        operationHr = dataSpaces->ReadVirtual(instruction, bytes.data(), static_cast<ULONG>(bytes.size()), &bytesRead);
        printHr("ReadVirtual(instruction)", operationHr);
        std::printf("  bytes-read=%lu data=", static_cast<unsigned long>(bytesRead));
        for(ULONG i = 0; i < bytesRead; ++i)
            std::printf("%02x", bytes[i]);
        std::putchar('\n');

        printLastEvent(control);
        printThreads(systemObjects);
        printModules(symbols);
        printLoadedRuntimeModules();

        for(int i = 2; i < argc; ++i)
        {
            ULONG status = DEBUG_STATUS_NO_CHANGE;
            if(!std::wcscmp(argv[i], L"--forward-step")) status = DEBUG_STATUS_STEP_INTO;
            else if(!std::wcscmp(argv[i], L"--reverse-step")) status = DEBUG_STATUS_REVERSE_STEP_INTO;
            else if(!std::wcscmp(argv[i], L"--forward-go")) status = DEBUG_STATUS_GO;
            else if(!std::wcscmp(argv[i], L"--reverse-go")) status = DEBUG_STATUS_REVERSE_GO;
            else
            {
                std::fwprintf(stderr, L"unknown option: %ls\n", argv[i]);
                return E_INVALIDARG;
            }
            operationHr = exerciseMovement(control, registers, status);
            if(FAILED(operationHr))
                return operationHr;
        }
        return S_OK;
    }();

    if(client)
    {
        const auto endHr = client->EndSession(DEBUG_END_PASSIVE);
        printHr("EndSession(DEBUG_END_PASSIVE)", endHr);
        client->SetOutputCallbacksWide(nullptr);
    }
    if(output)
        output->Release();
    release(registers);
    release(dataSpaces);
    release(symbols);
    release(systemObjects);
    release(control);
    release(client);
    if(uninitializeCom)
        CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
