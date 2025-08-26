# Simple Command-Line Debugger

A simple command-line debugger built using Microsoft's DbgEng API. This debugger provides basic debugging functionality for Windows executables.

## Features

- Process launching and debugging
- Breakpoint management (set/clear)
- Execution control (run, step over, step into)
- Register inspection
- Memory reading with hex output
- Event-driven debugging with proper callback handling

## Building

The project uses CMake for building:

```bash
cmake -B build
cmake --build build
```

**Important**: After building, copy the DbgEng DLLs to the output directory:

```bash
copy dbgeng\*.dll build\Debug\
```

## Usage

```bash
build\Debug\playground.exe <executable_path>
```

Example:
```bash
build\Debug\playground.exe build\Debug\testapp.exe
```

## Commands

The debugger supports the following commands:

### Execution Control

- **`run`** or **`r`** - Resume execution until next breakpoint or program termination
- **`sto`** - Step over (execute one instruction, stepping over function calls)
- **`sti`** - Step into (execute one instruction, stepping into function calls)

### Breakpoint Management

- **`bp 0xaddress`** - Set a breakpoint at the specified address
  - Example: `bp 0x140001234`
- **`bc 0xaddress`** - Clear/remove a breakpoint at the specified address
  - Example: `bc 0x140001234`

### Information Commands

- **`regs`** - Display current register values
  - Shows common x64 registers: rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8-r15, rip
- **`read 0xaddress size`** - Read memory at specified address
  - Example: `read 0x140001000 64`
  - Displays memory in hex format, 16 bytes per row

### Utility Commands

- **`help`** or **`h`** - Show available commands
- **`quit`** or **`q`** - Exit the debugger

## Example Session

```
> build\Debug\playground.exe build\Debug\testapp.exe
Process created: testapp.exe (base: 0x0000000140000000)
Initial breakpoint hit at 0x00007fff900e07a0
Debugger started. Process created.
Type 'help' for available commands.
(dbg) bp 0x140001234
Breakpoint set at 0x0000000140001234
(dbg) run
Running...
Breakpoint hit at 0x0000000140001234
(dbg) regs
Registers:
rax = 0x0000000000000000
rbx = 0x0000000000000000
rcx = 0x0000000000000001
rdx = 0x0000000000000000
rsi = 0x0000000000000000
rdi = 0x0000000000000000
rbp = 0x0000000000000000
rsp = 0x000000000012ff40
r8  = 0x0000000000000000
r9  = 0x0000000000000000
r10 = 0x0000000000000000
r11 = 0x0000000000000000
r12 = 0x0000000000000000
r13 = 0x0000000000000000
r14 = 0x0000000000000000
r15 = 0x0000000000000000
rip = 0x0000000140001234
(dbg) read 0x140001234 32
0000000140001234: 48 89 5c 24 08 48 89 74 24 10 57 48 83 ec 20 48
0000000140001244: 8b da 48 8b f1 48 8b 0d 45 2f 00 00 48 85 c9 75
(dbg) sto
Single step completed at 0x0000000140001238
(dbg) bc 0x140001234
Breakpoint removed at 0x0000000140001234
(dbg) quit
```

## Architecture

The debugger is built using the following components:

### Core Interfaces
- **IDebugClient9** - Primary interface for session management
- **IDebugControl** - Execution control and command processing
- **IDebugDataSpaces** - Memory access operations
- **IDebugRegisters** - Register read/write operations
- **IDebugSymbols** - Symbol resolution
- **IDebugSystemObjects** - Process/thread management

### Event Handling
- **DebugEventCallbacks** - Handles debug events (breakpoints, exceptions, process events)
- Proper event callback implementation with appropriate return values
- Automatic detection of breakpoint hits, single steps, and exceptions

### Command Processing
- Simple command parser with case-insensitive commands
- Address parsing supporting both `0x` prefixed and plain hex addresses
- Error handling for invalid commands and parameters

## Implementation Details

### Memory Layout
- Uses 64-bit addressing throughout for compatibility
- Memory reads display 16 bytes per row in hex format
- Proper error handling for invalid memory addresses

### Breakpoint Management
- Software breakpoints using `DEBUG_BREAKPOINT_CODE`
- Automatic breakpoint tracking and cleanup
- Support for multiple breakpoints

### Error Handling
- Comprehensive HRESULT checking for all DbgEng API calls
- Meaningful error messages for common failure scenarios
- Graceful cleanup on errors and exit

### Threading
- Single-threaded design following DbgEng requirements
- Proper event loop with `WaitForEvent()` calls
- Synchronous command execution

## Limitations

This is a simple prototype debugger with the following limitations:

- No symbol loading or symbolic breakpoints
- No disassembly display
- No stack trace functionality
- No multi-threading support
- No remote debugging capabilities
- Limited to Windows x64 targets

## Dependencies

- Microsoft DbgEng API (Windows Debugging Tools)
- Visual Studio 2022 or compatible C++ compiler
- CMake 3.15 or later
- Windows 10/11

## Files

- **`src/playground/playground.cpp`** - Main debugger implementation
- **`src/testapp/testapp.cpp`** - Test application for debugging
- **`dbgeng/`** - Required DbgEng DLLs
- **`CMakeLists.txt`** - Build configuration

## Troubleshooting

### "Failed to create IDebugClient9" Error
- Ensure DbgEng DLLs are copied to the output directory
- Check that you're running on a supported Windows version
- Verify Visual C++ Redistributable is installed

### Breakpoints Not Hit
- Ensure the address is valid and executable
- Check that the process is actually reaching that code path
- Verify the address format (use `0x` prefix)

### Memory Read Failures
- Verify the address is valid and accessible
- Check that the process is still running
- Ensure the size parameter is reasonable (1-1024 bytes)