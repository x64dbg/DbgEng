#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <Windows.h>
#include <format>

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

template<class... Args>
void logDebug(const std::format_string<Args...> fmt, Args&&... args)
{
    printf("[debug] %s\n", std::format(fmt, std::forward<Args>(args)...).c_str());
}

template<class...Args>
void print(const std::format_string<Args...> fmt, Args&&... args)
{
    puts(std::format(fmt, std::forward<Args>(args)...).c_str());
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

// Forward declarations
class SimpleDebugger;

// Event callback class
class DebugEventCallbacks : public IDebugEventCallbacksWide
{
private:
    SimpleDebugger* m_debugger;
    ULONG m_refCount;

public:
    DebugEventCallbacks(SimpleDebugger* debugger)
        : m_debugger(debugger), m_refCount(1) {}

    // IUnknown methods
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refCount); }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG count = InterlockedDecrement(&m_refCount);
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
        ULONG64 offset;
        if (SUCCEEDED(Bp->GetOffset(&offset)))
        {
            logDebug("Breakpoint hit at {:#x}", offset);
        }
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(Exception)(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) override
    {
        if (Exception->ExceptionCode == STATUS_BREAKPOINT)
        {
            logDebug("Initial breakpoint hit at {:#x}", Exception->ExceptionAddress);
        }
        else if (Exception->ExceptionCode == STATUS_SINGLE_STEP)
        {
            logDebug("Single step completed at {:#x}", Exception->ExceptionAddress);
        }
        else if (Exception->ExceptionCode == STATUS_ACCESS_VIOLATION)
        {
            logDebug("Access violation at {:#x}", Exception->ExceptionAddress);
        }
        else
        {
            logDebug("Exception {:#x} at {:#x}", Exception->ExceptionCode, Exception->ExceptionAddress);
        }

        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateThread)(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override
    {
        logDebug("Thread created - Handle: {:#x}, DataOffset: {:#x}, StartOffset: {:#x}",
            Handle,
            DataOffset,
            StartOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitThread)(ULONG ExitCode) override
    {
        logDebug("Thread exited with code: {} ({:#x})", ExitCode, ExitCode);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(CreateProcess)(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp, ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset, ULONG64 StartOffset) override
    {
        logDebug("Process created: {} (base: {:#x})",
            ImageName ? Utf16ToUtf8(ImageName) : "Unknown", BaseOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitProcess)(ULONG ExitCode) override
    {
        logDebug("Process exited with code: {}", ExitCode);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(LoadModule)(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize, PCWSTR ModuleName, PCWSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp) override
    {
        logDebug("Module loaded: {} ({}) - Base: {:#x}, Size: {:#x}, Checksum: {:#x}, Timestamp: {:#x}",
            ImageName ? Utf16ToUtf8(ImageName) : "Unknown",
            ModuleName ? Utf16ToUtf8(ModuleName) : "Unknown",
            BaseOffset,
            ModuleSize,
            CheckSum,
            TimeDateStamp);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(UnloadModule)(PCWSTR ImageBaseName, ULONG64 BaseOffset) override
    {
        logDebug("Module unloaded: {} - Base: {:#x}",
            ImageBaseName ? Utf16ToUtf8(ImageBaseName) : "Unknown",
            BaseOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SystemError)(ULONG Error, ULONG Level) override
    {
        logDebug("System error occurred - Error: {} ({:#x}), Level: {}",
            Error,
            Error,
            Level);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SessionStatus)(ULONG Status) override
    {
        const char* statusStr = "Unknown";
        switch (Status)
        {
        case DEBUG_SESSION_ACTIVE:
            statusStr = "Active";
            break;
        case DEBUG_SESSION_END_SESSION_ACTIVE_TERMINATE:
            statusStr = "End Session Active Terminate";
            break;
        case DEBUG_SESSION_END_SESSION_ACTIVE_DETACH:
            statusStr = "End Session Active Detach";
            break;
        case DEBUG_SESSION_END_SESSION_PASSIVE:
            statusStr = "End Session Passive";
            break;
        case DEBUG_SESSION_END:
            statusStr = "End";
            break;
        case DEBUG_SESSION_REBOOT:
            statusStr = "Reboot";
            break;
        case DEBUG_SESSION_HIBERNATE:
            statusStr = "Hibernate";
            break;
        case DEBUG_SESSION_FAILURE:
            statusStr = "Failure";
            break;
        }
        logDebug("Session status changed: {} ({})", statusStr, Status);
        return S_OK;
    }

    STDMETHOD(ChangeDebuggeeState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("Debuggee state changed - Flags: {:#x}, Argument: {:#x}",
            Flags,
            Argument);

        // Decode common flags for better understanding
        switch (Flags)
        {
        case DEBUG_CDS_ALL:
            logDebug("  - A general change in the target has occurred.");
            break;
        case DEBUG_CDS_REGISTERS:
            logDebug("  - Registers changed");
            break;
        case DEBUG_CDS_DATA:
            logDebug("  - Data/memory changed");
            break;
        case DEBUG_CDS_REFRESH:
            logDebug("  - Refresh requested");
            break;
        default:
            logDebug("  - Unknown change");
            break;
        }

        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ChangeEngineState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("Engine state changed - Flags: {:#x}, Argument: {:#x}",
            Flags,
            Argument);

        // Decode common flags for better understanding
        if (Flags & DEBUG_CES_CURRENT_THREAD)
            logDebug("  - Current thread changed");
        if (Flags & DEBUG_CES_EFFECTIVE_PROCESSOR)
            logDebug("  - Effective processor changed");
        if (Flags & DEBUG_CES_BREAKPOINTS)
            logDebug("  - Breakpoints changed");
        if (Flags & DEBUG_CES_CODE_LEVEL)
            logDebug("  - Code level changed");
        if (Flags & DEBUG_CES_EXECUTION_STATUS)
            logDebug("  - Execution status changed");
        if (Flags & DEBUG_CES_ENGINE_OPTIONS)
            logDebug("  - Engine options changed");
        if (Flags & DEBUG_CES_LOG_FILE)
            logDebug("  - Log file changed");
        if (Flags & DEBUG_CES_RADIX)
            logDebug("  - Radix changed");
        if (Flags & DEBUG_CES_EVENT_FILTERS)
            logDebug("  - Event filters changed");
        if (Flags & DEBUG_CES_PROCESS_OPTIONS)
            logDebug("  - Process options changed");
        if (Flags & DEBUG_CES_EXTENSIONS)
            logDebug("  - Extensions changed");
        if (Flags & DEBUG_CES_SYSTEMS)
            logDebug("  - Systems changed");
        if (Flags & DEBUG_CES_ASSEMBLY_OPTIONS)
            logDebug("  - Assembly options changed");
        if (Flags & DEBUG_CES_EXPRESSION_SYNTAX)
            logDebug("  - Expression syntax changed");
        if (Flags & DEBUG_CES_TEXT_REPLACEMENTS)
            logDebug("  - Text replacements changed");

        return S_OK;
    }

    STDMETHOD(ChangeSymbolState)(ULONG Flags, ULONG64 Argument) override
    {
        logDebug("Symbol state changed - Flags: {:#x}, Argument: {:#x}",
            Flags,
            Argument);

        // Decode common flags for better understanding
        if (Flags & DEBUG_CSS_LOADS)
            logDebug("  - Symbol loads changed");
        if (Flags & DEBUG_CSS_UNLOADS)
            logDebug("  - Symbol unloads changed");
        if (Flags & DEBUG_CSS_SCOPE)
            logDebug("  - Symbol scope changed");
        if (Flags & DEBUG_CSS_PATHS)
            logDebug("  - Symbol paths changed");
        if (Flags & DEBUG_CSS_SYMBOL_OPTIONS)
            logDebug("  - Symbol options changed");
        if (Flags & DEBUG_CSS_TYPE_OPTIONS)
            logDebug("  - Type options changed");
        if (Flags & DEBUG_CSS_COLLAPSE_CHILDREN)
            logDebug("  - Collapse children changed");

        return DEBUG_STATUS_NO_CHANGE;
    }
};

// Main debugger class
class SimpleDebugger
{
private:
    IDebugClient9* m_debugClient;
    IDebugControl* m_debugControl;
    IDebugDataSpaces* m_debugDataSpaces;
    IDebugRegisters* m_debugRegisters;
    IDebugSymbols* m_debugSymbols;
    IDebugSystemObjects* m_debugSystemObjects;

    DebugEventCallbacks* m_eventCallbacks;
    std::vector<IDebugBreakpoint*> m_breakpoints;
    bool m_debugActive;
    bool m_processRunning;

public:
    SimpleDebugger()
        : m_debugClient(nullptr), m_debugControl(nullptr), m_debugDataSpaces(nullptr), m_debugRegisters(nullptr), m_debugSymbols(nullptr), m_debugSystemObjects(nullptr), m_eventCallbacks(nullptr), m_debugActive(false), m_processRunning(false)
    {
    }

    ~SimpleDebugger()
    {
        Cleanup();
    }

    bool Initialize()
    {
        // Load dbgeng.dll
        LoadLibraryA("dbgeng.dll");

        // Create primary debug client interface
        HRESULT hr = DebugCreate(__uuidof(IDebugClient9), reinterpret_cast<void**>(&m_debugClient));
        if (FAILED(hr))
        {
            print("Failed to create IDebugClient9: {:#x}", (uint32_t)hr);
            return false;
        }

        // Query for other interfaces
        hr = m_debugClient->QueryInterface(__uuidof(IDebugControl), reinterpret_cast<void**>(&m_debugControl));
        if (FAILED(hr))
        {
            print("Failed to get IDebugControl: {:#x}", (uint32_t)hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugDataSpaces), reinterpret_cast<void**>(&m_debugDataSpaces));
        if (FAILED(hr))
        {
            print("Failed to get IDebugDataSpaces: {:#x}", (uint32_t)hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugRegisters), reinterpret_cast<void**>(&m_debugRegisters));
        if (FAILED(hr))
        {
            print("Failed to get IDebugRegisters: {:#x}", (uint32_t)hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugSymbols), reinterpret_cast<void**>(&m_debugSymbols));
        if (FAILED(hr))
        {
            print("Failed to get IDebugSymbols: {:#x}", (uint32_t)hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugSystemObjects), reinterpret_cast<void**>(&m_debugSystemObjects));
        if (FAILED(hr))
        {
            print("Failed to get IDebugSystemObjects: {:#x}", (uint32_t)hr);
            return false;
        }

        // Set up event callbacks
        m_eventCallbacks = new DebugEventCallbacks(this);
        hr = m_debugClient->SetEventCallbacksWide(m_eventCallbacks);
        if (FAILED(hr))
        {
            print("Failed to set event callbacks: {:#x}", (uint32_t)hr);
            return false;
        }

        // Set engine options for initial break
        hr = m_debugControl->SetEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK | DEBUG_ENGOPT_DISABLE_MODULE_SYMBOL_LOAD);
        if (FAILED(hr))
        {
            print("Failed to set engine options: {:#x}", (uint32_t)hr);
            return false;
        }

        m_debugActive = true;
        return true;
    }

    bool LaunchProcess(const char* executablePath)
    {
        if (!m_debugActive)
        {
            print("Debugger not initialized");
            return false;
        }

        HRESULT hr = m_debugClient->CreateProcess(0, const_cast<char*>(executablePath), DEBUG_ONLY_THIS_PROCESS);
        if (FAILED(hr))
        {
            print("Failed to create process: {:#x}", (uint32_t)hr);
            return false;
        }

        // Wait for initial event
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            print("Failed to wait for initial event: {:#x}", (uint32_t)hr);
            return false;
        }

        m_processRunning = true;
        print("Debugger started. Process created.");
        return true;
    }

    bool ExecuteCommand(const std::string& command)
    {
        std::istringstream iss(command);
        std::string cmd;
        iss >> cmd;

        // Convert to lowercase for comparison
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "run" || cmd == "r")
        {
            return CmdRun();
        }
        else if (cmd == "bp")
        {
            std::string addrStr;
            iss >> addrStr;
            return CmdSetBreakpoint(addrStr);
        }
        else if (cmd == "bc")
        {
            std::string addrStr;
            iss >> addrStr;
            return CmdClearBreakpoint(addrStr);
        }
        else if (cmd == "sto")
        {
            return CmdStepOver();
        }
        else if (cmd == "sti")
        {
            return CmdStepInto();
        }
        else if (cmd == "regs")
        {
            return CmdShowRegisters();
        }
        else if (cmd == "read")
        {
            std::string addrStr, sizeStr;
            iss >> addrStr >> sizeStr;
            return CmdReadMemory(addrStr, sizeStr);
        }
        else if (cmd == "quit" || cmd == "q")
        {
            return false; // Exit command loop
        }
        else if (cmd == "help" || cmd == "h")
        {
            ShowHelp();
            return true;
        }
        else
        {
            print("Unknown command: {} (type 'help' for available commands)", cmd);
            return true;
        }
    }

private:
    bool CmdRun()
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_GO);
        if (FAILED(hr))
        {
            print("Failed to resume execution: {:#x}", (uint32_t)hr);
            return true;
        }

        print("Running...");

        // Wait for next event
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            print("WaitForEvent failed: {:#x}", (uint32_t)hr);
            return true;
        }

        // Check if process is still running
        ULONG execStatus;
        hr = m_debugControl->GetExecutionStatus(&execStatus);
        if (SUCCEEDED(hr) && execStatus == DEBUG_STATUS_NO_DEBUGGEE)
        {
            m_processRunning = false;
        }

        return true;
    }

    bool CmdSetBreakpoint(const std::string& addrStr)
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            print("Invalid address: {}", addrStr);
            return true;
        }

        IDebugBreakpoint* bp;
        HRESULT hr = m_debugControl->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
        if (FAILED(hr))
        {
            print("Failed to add breakpoint: {:#x}", (uint32_t)hr);
            return true;
        }

        hr = bp->SetOffset(address);
        if (FAILED(hr))
        {
            print("Failed to set breakpoint offset: {:#x}", (uint32_t)hr);
            bp->Release();
            return true;
        }

        hr = bp->SetFlags(DEBUG_BREAKPOINT_ENABLED);
        if (FAILED(hr))
        {
            print("Failed to enable breakpoint: {:#x}", (uint32_t)hr);
            bp->Release();
            return true;
        }

        m_breakpoints.push_back(bp);
        print("Breakpoint set at {:#x}", address);
        return true;
    }

    bool CmdClearBreakpoint(const std::string& addrStr)
    {
        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            print("Invalid address: {}", addrStr);
            return true;
        }

        for (auto it = m_breakpoints.begin(); it != m_breakpoints.end(); ++it)
        {
            ULONG64 bpAddr;
            if (SUCCEEDED((*it)->GetOffset(&bpAddr)) && bpAddr == address)
            {
                HRESULT hr = m_debugControl->RemoveBreakpoint(*it);
                if (SUCCEEDED(hr))
                {
                    print("Breakpoint removed at {:#x}", address);
                }
                else
                {
                    print("Failed to remove breakpoint: {:#x}", (uint32_t)hr);
                }
                (*it)->Release();
                m_breakpoints.erase(it);
                return true;
            }
        }

        print("No breakpoint found at {:#x}", address);
        return true;
    }

    bool CmdStepOver()
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_STEP_OVER);
        if (FAILED(hr))
        {
            print("Failed to step over: {:#x}", (uint32_t)hr);
            return true;
        }

        // Wait for step to complete
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            print("WaitForEvent failed: {:#x}", (uint32_t)hr);
        }

        return true;
    }

    bool CmdStepInto()
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_STEP_INTO);
        if (FAILED(hr))
        {
            print("Failed to step into: {:#x}", (uint32_t)hr);
            return true;
        }

        // Wait for step to complete
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            print("WaitForEvent failed: {:#x}", (uint32_t)hr);
        }

        return true;
    }

    bool CmdShowRegisters()
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        ULONG numRegs;
        HRESULT hr = m_debugRegisters->GetNumberRegisters(&numRegs);
        if (FAILED(hr))
        {
            print("Failed to get register count: {:#x}", (uint32_t)hr);
            return true;
        }

        print("Registers:");

        // Common x64 registers to display
        const char* commonRegs[] = {
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"
        };

        for (const char* regName : commonRegs)
        {
            ULONG regIndex;
            hr = m_debugRegisters->GetIndexByName(regName, &regIndex);
            if (SUCCEEDED(hr))
            {
                DEBUG_VALUE regValue;
                hr = m_debugRegisters->GetValue(regIndex, &regValue);
                if (SUCCEEDED(hr))
                {
                    print("{:<3} = {:#x}", regName, regValue.I64);
                }
            }
        }

        return true;
    }

    bool CmdReadMemory(const std::string& addrStr, const std::string& sizeStr)
    {
        if (!m_processRunning)
        {
            print("No process running");
            return true;
        }

        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            print("Invalid address: {}", addrStr);
            return true;
        }

        ULONG size = static_cast<ULONG>(std::stoul(sizeStr, nullptr, 0));
        if (size == 0 || size > 1024)
        {
            print("Invalid size: {} (must be 1-1024)", sizeStr);
            return true;
        }

        std::vector<UCHAR> buffer(size);
        ULONG bytesRead;

        HRESULT hr = m_debugDataSpaces->ReadVirtual(address, buffer.data(), size, &bytesRead);
        if (FAILED(hr))
        {
            print("Failed to read memory: {:#x}", (uint32_t)hr);
            return true;
        }

        // Display memory in hex format, 16 bytes per row
        for (ULONG i = 0; i < bytesRead; i += 16)
        {
            std::string hexLine = std::format("{:016x}: ", address + i);

            // Print hex bytes
            for (ULONG j = 0; j < 16 && (i + j) < bytesRead; j++)
            {
                hexLine += std::format("{:02x} ", buffer[i + j]);
            }

            print("{}", hexLine);
        }

        return true;
    }

    ULONG64 ParseAddress(const std::string& addrStr)
    {
        try
        {
            if (addrStr.length() > 2 && addrStr.substr(0, 2) == "0x")
            {
                return std::stoull(addrStr, nullptr, 16);
            }
            else
            {
                return std::stoull(addrStr, nullptr, 16);
            }
        }
        catch (...)
        {
            return 0;
        }
    }

    void ShowHelp()
    {
        print("Available commands:");
        print("  run, r          - Resume execution");
        print("  bp 0xaddr       - Set breakpoint at address");
        print("  bc 0xaddr       - Clear breakpoint at address");
        print("  sto             - Step over (instruction)");
        print("  sti             - Step into (instruction)");
        print("  regs            - Show registers");
        print("  read 0xaddr size - Read memory at address");
        print("  help, h         - Show this help");
        print("  quit, q         - Exit debugger");
    }

    void Cleanup()
    {
        // Clean up breakpoints
        for (auto bp : m_breakpoints)
        {
            if (bp)
                bp->Release();
        }
        m_breakpoints.clear();

        // End debug session
        if (m_debugClient)
        {
            m_debugClient->EndSession(DEBUG_END_PASSIVE);
        }

        // Release interfaces in reverse order
        if (m_debugSystemObjects)
        {
            m_debugSystemObjects->Release();
            m_debugSystemObjects = nullptr;
        }
        if (m_debugSymbols)
        {
            m_debugSymbols->Release();
            m_debugSymbols = nullptr;
        }
        if (m_debugRegisters)
        {
            m_debugRegisters->Release();
            m_debugRegisters = nullptr;
        }
        if (m_debugDataSpaces)
        {
            m_debugDataSpaces->Release();
            m_debugDataSpaces = nullptr;
        }
        if (m_debugControl)
        {
            m_debugControl->Release();
            m_debugControl = nullptr;
        }
        if (m_debugClient)
        {
            m_debugClient->Release();
            m_debugClient = nullptr;
        }

        if (m_eventCallbacks)
        {
            m_eventCallbacks->Release();
            m_eventCallbacks = nullptr;
        }

        m_debugActive = false;
        m_processRunning = false;
    }

public:
    void RunCommandLoop()
    {
        std::string command;
        printf("(dbg) ");

        while (std::getline(std::cin, command))
        {
            if (!command.empty())
            {
                if (!ExecuteCommand(command))
                {
                    break; // Exit requested
                }
            }

            if (m_processRunning)
            {
                printf("(dbg) ");
            }
            else
            {
                puts("Process terminated. Type 'quit' to exit.");
                printf("(dbg) ");
            }
        }
    }
};

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        puts(std::format("Usage: {} <executable_path>", argv[0]).c_str());
        print("Example: {} build\\testapp.exe", argv[0]);
        return EXIT_FAILURE;
    }

    SimpleDebugger debugger;

    if (!debugger.Initialize())
    {
        print("Failed to initialize debugger");
        return EXIT_FAILURE;
    }

    if (!debugger.LaunchProcess(argv[1]))
    {
        print("Failed to launch process: {}", argv[1]);
        return EXIT_FAILURE;
    }

    print("Type 'help' for available commands.");
    debugger.RunCommandLoop();

    return EXIT_SUCCESS;
}
