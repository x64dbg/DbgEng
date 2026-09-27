#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

#include <TTD/IReplayEngine.h>
#include <TTD/IReplayEngineStl.h>

// Thin adapter over the official TTD replay SDK (third_party/TTD).
namespace TTDReplay {

using namespace TTD;
using namespace TTD::Replay;

// Loads TTDReplay.dll (next to this module, then the default search path) and
// creates a replay engine through the exported CreateReplayEngine entry point.
// The library stays loaded for the lifetime of the process, so engines and
// cursors can be destroyed at any time. Returns null and sets the Win32 last
// error on failure.
UniqueReplayEngine CreateEngine();

// Reads up to size bytes at address from the cursor's current position and
// returns the number of contiguous bytes that were available.
size_t ReadMemoryPartial(const ICursorView& cursor, uint64_t address, void* buffer, size_t size);
bool ReadMemory(const ICursorView& cursor, uint64_t address, void* buffer, size_t size);

} // namespace TTDReplay
