# TTD replay SDK

Headers from the official `Microsoft.TimeTravelDebugging.Apis` NuGet package,
version 0.9.5 (MIT licensed, https://aka.ms/TTD). Only `sdk/include/TTD` is
vendored; the import libraries are not used because `src/TTD/TTD.cpp` resolves
`CreateReplayEngine` from `TTDReplay.dll` at runtime.

The matching runtime (`TTDReplay.dll`, `TTDReplayCPU.dll`, 1.11.611.0) lives in
the `deps` repository under `x64/DbgEng` and `x32/DbgEng`. It comes from
https://aka.ms/ttd/download (the `TTD-x64` / `TTD-x86` MSIX packages), extracted
with `Get-Ttd.ps1` from https://github.com/microsoft/WinDbg-Samples.
