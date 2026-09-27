#include "TTD.hpp"

#include <algorithm>

namespace TTDReplay {

using CreateReplayEngineProc = uint32_t(__cdecl*)(IReplayEngine*&, const GUID&);

static HMODULE LoadReplayModule()
{
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadReplayModule), &owner))
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(owner, path, ARRAYSIZE(path)))
        {
            if (auto slash = wcsrchr(path, L'\\'))
            {
                wcscpy_s(slash + 1, ARRAYSIZE(path) - (slash + 1 - path), L"TTDReplay.dll");
                if (auto replay = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                    return replay;
            }
        }
    }
    return LoadLibraryW(L"TTDReplay.dll");
}

UniqueReplayEngine CreateEngine()
{
    static const HMODULE module = LoadReplayModule();
    if (!module)
        return nullptr;

    const auto create = reinterpret_cast<CreateReplayEngineProc>(GetProcAddress(module, "CreateReplayEngine"));
    if (!create)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    IReplayEngine* engine = nullptr;
    if (create(engine, __uuidof(IReplayEngineView)) != 0 || !engine)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return nullptr;
    }
    return UniqueReplayEngine(engine);
}

size_t ReadMemoryPartial(const ICursorView& cursor, uint64_t address, void* buffer, size_t size)
{
    size_t total = 0;
    while (total < size)
    {
        const auto current = address + total;
        const BufferView destination(static_cast<uint8_t*>(buffer) + total, size - total);
        const auto result = cursor.QueryMemoryBuffer(static_cast<GuestAddress>(current), destination);
        if (!result.Memory.BaseAddress || static_cast<uint64_t>(result.Address) != current || !result.Memory.Size)
            break;
        total += (std::min)(size - total, result.Memory.Size);
    }
    return total;
}

bool ReadMemory(const ICursorView& cursor, uint64_t address, void* buffer, size_t size)
{
    return ReadMemoryPartial(cursor, address, buffer, size) == size;
}

} // namespace TTDReplay
