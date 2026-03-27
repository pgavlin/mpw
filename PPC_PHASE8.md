# Phase 8: Additional InterfaceLib Wrappers for Tools

**Goal:** Implement InterfaceLib wrappers for symbols that PPC tools (other than StdCLib) import directly.

**Depends on:** Phases 5-7 (Hello, world! runs end-to-end).

**Status:** Not started. Hello tool works with the 66 StdCLib imports from Phase 4. Additional wrappers will be needed as more complex tools are tested.

---

## Overview

Phase 4 implements exactly the 66 symbols StdCLib imports. PPC tools may import additional symbols from InterfaceLib directly. The catch-all handler identifies missing stubs at runtime:
```
PPC FATAL: unimplemented stub InterfaceLib::SomeFunction called
  r3=0x... r4=0x... r5=0x... LR=0x...
```

This phase is demand-driven — add wrappers as tools require them. Use `--debug` to set breakpoints and inspect state when a missing stub fires.

---

## Current State

The Hello tool imports 10 symbols from StdCLib (fprintf, exit, __setjmp, _BreakPoint, _IntEnv, __C_phase, __target_for_exit, _exit_status, _iob, __NubAt3), all of which are resolved via StdCLib's exports. StdCLib in turn imports 66 symbols from InterfaceLib/MathLib/PrivateInterfaceLib, all implemented in Phase 4.

More complex tools (compilers, linkers, DumpPEF, etc.) will import directly from InterfaceLib. Use `mpw DumpPEF -do All -pi u -a -fmt on <tool>` to see what a tool needs before running it.

---

## Known Working Infrastructure

These subsystems are validated end-to-end and can be built upon:

| Subsystem | Status |
|-----------|--------|
| PPC CPU (Unicorn) | Working. MSR[FP]=1 for FP instructions. |
| PEF loader | Working. pidata decompression, full relocation engine. |
| CFM stubs (sc dispatch) | Working. On-demand library loading. |
| Memory Manager (NewPtr/NewHandle/HLock/etc.) | Working. Note: PPC SP must be in stack area, not pool area. |
| ECON device handlers | Working. read/write/close/ioctl/faccess. |
| Handle-based cookies | Working. Proper NewHandle allocation. |
| CallUniversalProc trampoline | Working. PPC TVector dispatch. |
| Interactive debugger | Working. PPC step/break/registers/disassembly. |
| pef_inspect tool | Working. Structural queries on PEF binaries. |

---

## Likely Additional Symbols

### Memory Manager extras

| Symbol | Implementation |
|---|---|
| `NewPtrClear` | `MM::Native::NewPtr(r3, true, ptr)` |
| `NewHandleClear` | `MM::Native::NewHandle(r3, true, h)` |
| `GetHandleSize` | `MM::Native::GetHandleSize(r3, sz)` |
| `SetHandleSize` | `MM::Native::SetHandleSize(r3, r4)` |
| `BlockMoveData` | Same as BlockMove |
| `HandleZone` | Return current zone |
| `HGetState` / `HSetState` | Return 0 / no-op |
| `HNoPurge` / `HPurge` | No-op |
| `MoveHHi` | No-op |

### Resource Manager extras

| Symbol | Implementation |
|---|---|
| `GetResource` | `RM::Native::GetResource(r3, r4, handle)` |
| `Get1Resource` | Get1Resource variant |
| `SetResLoad` | `RM::Native::SetResLoad(r3)` |
| `UseResFile` | `RM::Native::UseResFile(r3)` |
| `OpenResFile` | `RM::Native::OpenResFile(readPString(r3))` |
| `CloseResFile` | `RM::Native::CloseResFile(r3)` |
| `HomeResFile` | `RM::Native::HomeResFile(r3)` |
| `GetResAttrs` | `RM::Native::GetResAttrs(r3)` |
| `GetResInfo` | Bridge to `RM::Native::GetResInfo` |
| `GetResourceSizeOnDisk` | `RM::Native::GetResourceSizeOnDisk(r3)` |
| `AddResource` | `RM::Native::AddResource(r3, r4, r5, readPString(r6))` |
| `ChangedResource` | `RM::Native::ChangedResource(r3)` |
| `UpdateResFile` | `RM::Native::UpdateResFile(r3)` |
| `RemoveResource` | `RM::Native::RemoveResource(r3)` |
| `ReadPartialResource` | Custom implementation |

### File Manager extras

| Symbol | Implementation |
|---|---|
| `PBReadSync` | `OS::Native::Read(r3)` |
| `PBWriteSync` | `OS::Native::Write(r3)` |
| `GetToolTrapAddress` | Bridge to existing 68K handler |
| `GetOSTrapAddress` | Bridge to existing 68K handler |
| `ReadDateTime` | Same as GetDateTime |

### Misc extras

| Symbol | Implementation |
|---|---|
| `LMGetBootDrive` | `return memoryReadWord(0x0210)` |
| `FindFolder` | Return fnfErr (-43) |
| `numtostring` | Convert r3 to Pascal string at r4 |
| `GetCursor` / `SetCursor` / `ShowCursor` | No-op (CLI emulator) |

---

## Implementation

Add wrappers to `toolbox/ppc_dispatch.cpp` as a new registration function:

```cpp
namespace PPCDispatch {
    void RegisterStdCLibImports();  // Phase 4
    void RegisterToolImports();     // Phase 8
}
```

`RegisterToolImports()` is called after `RegisterStdCLibImports()` in the loader. Any symbol already registered by Phase 4 is skipped (RegisterStub returns the existing TVector).

---

## Validation

Run various PPC MPW tools and verify no catch-all stubs fire. Add wrappers incrementally as needed.

### Test progression
1. Simple tools (Hello — done ✓)
2. DumpPEF (PPC version, if available)
3. Compilers (SC, MrC)
4. Linkers (PPCLink)
