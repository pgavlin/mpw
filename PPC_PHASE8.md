# Phase 8: Additional InterfaceLib Wrappers for Tools

**Goal:** Implement InterfaceLib wrappers for symbols that PPC tools (other than StdCLib) import directly.

**Depends on:** Phase 4 (StdCLib wrappers), Phases 5-6 (end-to-end execution to discover which additional stubs are needed).

---

## Overview

Phase 4 implements exactly the 66 symbols StdCLib imports. When we begin running actual PPC tools in Phases 5-6, those tools may import additional symbols from InterfaceLib directly (not via StdCLib). This phase adds those wrappers as needed.

The catch-all handler from Phase 4 will identify missing stubs at runtime:
```
PPC FATAL: unimplemented stub InterfaceLib::SomeFunction called
  r3=0x... r4=0x... r5=0x... LR=0x...
```

This phase is demand-driven — we add wrappers as tools require them.

---

## Likely Additional Symbols

Based on the old `ppc-dead-end` branch and common MPW tool patterns, tools may import:

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

## Implementation Approach

Add wrappers to `toolbox/ppc_dispatch.cpp` as a new registration function:

```cpp
namespace PPCDispatch {
    void RegisterStdCLibImports();  // Phase 4
    void RegisterToolImports();     // Phase 8
}
```

`RegisterToolImports()` is called after `RegisterStdCLibImports()` in the Phase 5 loader. Any symbol already registered by Phase 4 is skipped (RegisterStub returns the existing TVector).

---

## Validation

Run various PPC MPW tools and verify no catch-all stubs fire. Add wrappers incrementally as needed.
