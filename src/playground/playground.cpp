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

// Forward declarations
class SimpleDebugger;

// Event callback class
class DebugEventCallbacks : public IDebugEventCallbacks
{
private:
    SimpleDebugger* m_debugger;
    ULONG m_refCount;

public:
    DebugEventCallbacks(SimpleDebugger* debugger) : m_debugger(debugger), m_refCount(1) {}

    // IUnknown methods
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&m_refCount); }
    STDMETHOD_(ULONG, Release)() override 
    { 
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDebugEventCallbacks))
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
                DEBUG_EVENT_CHANGE_SYMBOL_STATE;
        return S_OK;
    }

    STDMETHOD(Breakpoint)(PDEBUG_BREAKPOINT Bp) override
    {
        ULONG64 offset;
        if (SUCCEEDED(Bp->GetOffset(&offset)))
        {
            printf("Breakpoint hit at 0x%016llx\n", offset);
        }
        return DEBUG_STATUS_BREAK;
    }

    STDMETHOD(Exception)(PEXCEPTION_RECORD64 Exception, ULONG FirstChance) override
    {
        if (Exception->ExceptionCode == STATUS_BREAKPOINT)
        {
            printf("Initial breakpoint hit at 0x%016llx\n", Exception->ExceptionAddress);
            return DEBUG_STATUS_BREAK;
        }
        else if (Exception->ExceptionCode == STATUS_SINGLE_STEP)
        {
            printf("Single step completed at 0x%016llx\n", Exception->ExceptionAddress);
            return DEBUG_STATUS_BREAK;
        }
        else if (Exception->ExceptionCode == STATUS_ACCESS_VIOLATION)
        {
            printf("Access violation at 0x%016llx\n", Exception->ExceptionAddress);
            return DEBUG_STATUS_BREAK;
        }
        
        return FirstChance ? DEBUG_STATUS_NO_CHANGE : DEBUG_STATUS_BREAK;
    }

    STDMETHOD(CreateThread)(ULONG64 Handle, ULONG64 DataOffset, ULONG64 StartOffset) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitThread)(ULONG ExitCode) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(CreateProcess)(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset,
                           ULONG ModuleSize, PCSTR ModuleName, PCSTR ImageName,
                           ULONG CheckSum, ULONG TimeDateStamp, ULONG64 InitialThreadHandle,
                           ULONG64 ThreadDataOffset, ULONG64 StartOffset) override
    {
        printf("Process created: %s (base: 0x%016llx)\n", ImageName ? ImageName : "Unknown", BaseOffset);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ExitProcess)(ULONG ExitCode) override
    {
        printf("Process exited with code: %lu\n", ExitCode);
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(LoadModule)(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize,
                         PCSTR ModuleName, PCSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(UnloadModule)(PCSTR ImageBaseName, ULONG64 BaseOffset) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SystemError)(ULONG Error, ULONG Level) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(SessionStatus)(ULONG Status) override
    {
        return S_OK;
    }

    STDMETHOD(ChangeDebuggeeState)(ULONG Flags, ULONG64 Argument) override
    {
        return DEBUG_STATUS_NO_CHANGE;
    }

    STDMETHOD(ChangeEngineState)(ULONG Flags, ULONG64 Argument) override
    {
        return S_OK;
    }

    STDMETHOD(ChangeSymbolState)(ULONG Flags, ULONG64 Argument) override
    {
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
    SimpleDebugger() : m_debugClient(nullptr), m_debugControl(nullptr), 
                      m_debugDataSpaces(nullptr), m_debugRegisters(nullptr),
                      m_debugSymbols(nullptr), m_debugSystemObjects(nullptr),
                      m_eventCallbacks(nullptr), m_debugActive(false), m_processRunning(false)
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
            printf("Failed to create IDebugClient9: 0x%08x\n", hr);
            return false;
        }

        // Query for other interfaces
        hr = m_debugClient->QueryInterface(__uuidof(IDebugControl), reinterpret_cast<void**>(&m_debugControl));
        if (FAILED(hr))
        {
            printf("Failed to get IDebugControl: 0x%08x\n", hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugDataSpaces), reinterpret_cast<void**>(&m_debugDataSpaces));
        if (FAILED(hr))
        {
            printf("Failed to get IDebugDataSpaces: 0x%08x\n", hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugRegisters), reinterpret_cast<void**>(&m_debugRegisters));
        if (FAILED(hr))
        {
            printf("Failed to get IDebugRegisters: 0x%08x\n", hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugSymbols), reinterpret_cast<void**>(&m_debugSymbols));
        if (FAILED(hr))
        {
            printf("Failed to get IDebugSymbols: 0x%08x\n", hr);
            return false;
        }

        hr = m_debugClient->QueryInterface(__uuidof(IDebugSystemObjects), reinterpret_cast<void**>(&m_debugSystemObjects));
        if (FAILED(hr))
        {
            printf("Failed to get IDebugSystemObjects: 0x%08x\n", hr);
            return false;
        }

        // Set up event callbacks
        m_eventCallbacks = new DebugEventCallbacks(this);
        hr = m_debugClient->SetEventCallbacks(m_eventCallbacks);
        if (FAILED(hr))
        {
            printf("Failed to set event callbacks: 0x%08x\n", hr);
            return false;
        }

        // Set engine options for initial break
        hr = m_debugControl->SetEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK);
        if (FAILED(hr))
        {
            printf("Failed to set engine options: 0x%08x\n", hr);
            return false;
        }

        m_debugActive = true;
        return true;
    }

    bool LaunchProcess(const char* executablePath)
    {
        if (!m_debugActive)
        {
            printf("Debugger not initialized\n");
            return false;
        }

        HRESULT hr = m_debugClient->CreateProcess(0, const_cast<char*>(executablePath), 
                                                 DEBUG_ONLY_THIS_PROCESS);
        if (FAILED(hr))
        {
            printf("Failed to create process: 0x%08x\n", hr);
            return false;
        }

        // Wait for initial event
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            printf("Failed to wait for initial event: 0x%08x\n", hr);
            return false;
        }


        m_processRunning = true;
        printf("Debugger started. Process created.\n");
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
            printf("Unknown command: %s (type 'help' for available commands)\n", cmd.c_str());
            return true;
        }
    }

private:
    bool CmdRun()
    {
        if (!m_processRunning)
        {
            printf("No process running\n");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_GO);
        if (FAILED(hr))
        {
            printf("Failed to resume execution: 0x%08x\n", hr);
            return true;
        }

        printf("Running...\n");
        
        // Wait for next event
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            printf("WaitForEvent failed: 0x%08x\n", hr);
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
            printf("No process running\n");
            return true;
        }

        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            printf("Invalid address: %s\n", addrStr.c_str());
            return true;
        }

        IDebugBreakpoint* bp;
        HRESULT hr = m_debugControl->AddBreakpoint(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp);
        if (FAILED(hr))
        {
            printf("Failed to add breakpoint: 0x%08x\n", hr);
            return true;
        }

        hr = bp->SetOffset(address);
        if (FAILED(hr))
        {
            printf("Failed to set breakpoint offset: 0x%08x\n", hr);
            bp->Release();
            return true;
        }

        hr = bp->SetFlags(DEBUG_BREAKPOINT_ENABLED);
        if (FAILED(hr))
        {
            printf("Failed to enable breakpoint: 0x%08x\n", hr);
            bp->Release();
            return true;
        }

        m_breakpoints.push_back(bp);
        printf("Breakpoint set at 0x%016llx\n", address);
        return true;
    }

    bool CmdClearBreakpoint(const std::string& addrStr)
    {
        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            printf("Invalid address: %s\n", addrStr.c_str());
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
                    printf("Breakpoint removed at 0x%016llx\n", address);
                }
                else
                {
                    printf("Failed to remove breakpoint: 0x%08x\n", hr);
                }
                (*it)->Release();
                m_breakpoints.erase(it);
                return true;
            }
        }

        printf("No breakpoint found at 0x%016llx\n", address);
        return true;
    }

    bool CmdStepOver()
    {
        if (!m_processRunning)
        {
            printf("No process running\n");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_STEP_OVER);
        if (FAILED(hr))
        {
            printf("Failed to step over: 0x%08x\n", hr);
            return true;
        }

        // Wait for step to complete
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            printf("WaitForEvent failed: 0x%08x\n", hr);
        }

        return true;
    }

    bool CmdStepInto()
    {
        if (!m_processRunning)
        {
            printf("No process running\n");
            return true;
        }

        HRESULT hr = m_debugControl->SetExecutionStatus(DEBUG_STATUS_STEP_INTO);
        if (FAILED(hr))
        {
            printf("Failed to step into: 0x%08x\n", hr);
            return true;
        }

        // Wait for step to complete
        hr = m_debugControl->WaitForEvent(DEBUG_WAIT_DEFAULT, INFINITE);
        if (FAILED(hr))
        {
            printf("WaitForEvent failed: 0x%08x\n", hr);
        }

        return true;
    }

    bool CmdShowRegisters()
    {
        if (!m_processRunning)
        {
            printf("No process running\n");
            return true;
        }

        ULONG numRegs;
        HRESULT hr = m_debugRegisters->GetNumberRegisters(&numRegs);
        if (FAILED(hr))
        {
            printf("Failed to get register count: 0x%08x\n", hr);
            return true;
        }

        printf("Registers:\n");
        
        // Common x64 registers to display
        const char* commonRegs[] = {
            "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"
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
                    printf("%-3s = 0x%016llx\n", regName, regValue.I64);
                }
            }
        }

        return true;
    }

    bool CmdReadMemory(const std::string& addrStr, const std::string& sizeStr)
    {
        if (!m_processRunning)
        {
            printf("No process running\n");
            return true;
        }

        ULONG64 address = ParseAddress(addrStr);
        if (address == 0)
        {
            printf("Invalid address: %s\n", addrStr.c_str());
            return true;
        }

        ULONG size = static_cast<ULONG>(std::stoul(sizeStr, nullptr, 0));
        if (size == 0 || size > 1024)
        {
            printf("Invalid size: %s (must be 1-1024)\n", sizeStr.c_str());
            return true;
        }

        std::vector<UCHAR> buffer(size);
        ULONG bytesRead;
        
        HRESULT hr = m_debugDataSpaces->ReadVirtual(address, buffer.data(), size, &bytesRead);
        if (FAILED(hr))
        {
            printf("Failed to read memory: 0x%08x\n", hr);
            return true;
        }

        // Display memory in hex format, 16 bytes per row
        for (ULONG i = 0; i < bytesRead; i += 16)
        {
            printf("%016llx: ", address + i);
            
            // Print hex bytes
            for (ULONG j = 0; j < 16 && (i + j) < bytesRead; j++)
            {
                printf("%02x ", buffer[i + j]);
            }
            
            printf("\n");
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
        printf("Available commands:\n");
        printf("  run, r          - Resume execution\n");
        printf("  bp 0xaddr       - Set breakpoint at address\n");
        printf("  bc 0xaddr       - Clear breakpoint at address\n");
        printf("  sto             - Step over (instruction)\n");
        printf("  sti             - Step into (instruction)\n");
        printf("  regs            - Show registers\n");
        printf("  read 0xaddr size - Read memory at address\n");
        printf("  help, h         - Show this help\n");
        printf("  quit, q         - Exit debugger\n");
    }

    void Cleanup()
    {
        // Clean up breakpoints
        for (auto bp : m_breakpoints)
        {
            if (bp) bp->Release();
        }
        m_breakpoints.clear();

        // End debug session
        if (m_debugClient)
        {
            m_debugClient->EndSession(DEBUG_END_PASSIVE);
        }

        // Release interfaces in reverse order
        if (m_debugSystemObjects) { m_debugSystemObjects->Release(); m_debugSystemObjects = nullptr; }
        if (m_debugSymbols) { m_debugSymbols->Release(); m_debugSymbols = nullptr; }
        if (m_debugRegisters) { m_debugRegisters->Release(); m_debugRegisters = nullptr; }
        if (m_debugDataSpaces) { m_debugDataSpaces->Release(); m_debugDataSpaces = nullptr; }
        if (m_debugControl) { m_debugControl->Release(); m_debugControl = nullptr; }
        if (m_debugClient) { m_debugClient->Release(); m_debugClient = nullptr; }
        
        if (m_eventCallbacks) { m_eventCallbacks->Release(); m_eventCallbacks = nullptr; }

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
                printf("Process terminated. Type 'quit' to exit.\n");
                printf("(dbg) ");
            }
        }
    }
};

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        printf("Usage: %s <executable_path>\n", argv[0]);
        printf("Example: %s build\\testapp.exe\n", argv[0]);
        return EXIT_FAILURE;
    }

    SimpleDebugger debugger;
    
    if (!debugger.Initialize())
    {
        printf("Failed to initialize debugger\n");
        return EXIT_FAILURE;
    }

    if (!debugger.LaunchProcess(argv[1]))
    {
        printf("Failed to launch process: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    printf("Type 'help' for available commands.\n");
    debugger.RunCommandLoop();

    return EXIT_SUCCESS;
}
