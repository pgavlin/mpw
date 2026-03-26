# Phase 4: InterfaceLib Wrappers

**Goal:** Implement the InterfaceLib (and MathLib/PrivateInterfaceLib) functions that StdCLib imports, bridging PPC calling convention to existing `Native::` APIs.

**Depends on:** Phase 1 (PPC register access), Phase 2 (PEF loader), Phase 3 (CFM stub registration).

---

## Overview

As discovered in Phase 3 analysis, InterfaceLib, MathLib, and PrivateInterfaceLib are **stub libraries** — PEFs with no code or data, only export catalogs. On a real Mac, CFM resolves these exports against the Toolbox ROM. In our emulator, this phase provides the actual implementations.

We do NOT load InterfaceLib's PEF. Instead, we register a native C++ handler for each function that StdCLib (or the tool) actually imports. The CFM stub system from Phase 3 allocates a TVector with an `sc` instruction for each handler. When StdCLib calls `NewPtr`, it goes through a TVector that triggers our `sc` handler, which calls `MM::Native::NewPtr`.

StdCLib imports 60 symbols from InterfaceLib, 4 from MathLib, and 2 from PrivateInterfaceLib. Each wrapper:
1. Reads arguments from PPC registers (r3-r10 for integers, f1-f13 for floats)
2. Calls the corresponding existing `Native::` API or implements the function directly
3. Writes the return value to r3 (or f1 for floats)

The PPC calling convention: r3-r10 are parameters (left to right), r3 is the return value, f1-f13 for FP parameters, f1 for FP return.

---

## Function Categories

### Memory Manager (bridge to `MM::Native`)

These are simple — PPC functions take the same parameters as the Native:: APIs:

| InterfaceLib Symbol | PPC Args | Implementation |
|---|---|---|
| `NewPtr` | r3=size | `MM::Native::NewPtr(r3, false, ptr); return ptr` |
| `NewPtrClear` | r3=size | `MM::Native::NewPtr(r3, true, ptr); return ptr` |
| `DisposePtr` | r3=ptr | `MM::Native::DisposePtr(r3)` |
| `GetPtrSize` | r3=ptr | Read via 68K: `cpuSetAReg(0, r3); MM::GetPtrSize(0xA021); return cpuGetDReg(0)` |
| `SetPtrSize` | r3=ptr, r4=size | No-op, return 0 (noErr) |
| `NewHandle` | r3=size | `MM::Native::NewHandle(r3, false, h); return h` |
| `NewHandleClear` | r3=size | `MM::Native::NewHandle(r3, true, h); return h` |
| `DisposeHandle` | r3=handle | `MM::Native::DisposeHandle(r3)` |
| `GetHandleSize` | r3=handle | `MM::Native::GetHandleSize(r3, sz); return sz` |
| `SetHandleSize` | r3=handle, r4=size | `return MM::Native::SetHandleSize(r3, r4)` |
| `HLock` | r3=handle | `MM::Native::HLock(r3); return 0` |
| `HUnlock` | r3=handle | `MM::Native::HUnlock(r3); return 0` |
| `BlockMove` / `BlockMoveData` | r3=src, r4=dst, r5=size | `memmove(memPtr(r4), memPtr(r3), r5)` |
| `FreeMem` | (none) | `cpuSetDReg(0, 0); return MM::FreeMem(0)` |
| `MemError` | (none) | `return memoryReadWord(0x0220)` (MemErr low-memory global) |
| `GetZone` | (none) | `return memoryReadLong(MacOS::TheZone)` |
| `SetZone` | r3=zone | `memoryWriteLong(r3, MacOS::TheZone)` |
| `HandleZone` | r3=handle | Return current zone |
| `HGetState` / `HSetState` | r3=handle | Return 0 / no-op |
| `HNoPurge` / `HPurge` | r3=handle | No-op |
| `MoveHHi` | r3=handle | No-op |

### File Manager (bridge to `OS::Native`)

Most File Manager calls use parameter blocks. The PPC wrapper receives the parameter block pointer in r3 and passes it to the Native:: API:

| InterfaceLib Symbol | PPC Args | Implementation |
|---|---|---|
| `PBHOpenSync` | r3=pb | See special handling below |
| `PBHOpenRFSync` | r3=pb | `OS::Native::OpenRF(r3, 0xA200)` |
| `PBCloseSync` | r3=pb | `OS::Native::Close(r3)` |
| `PBReadSync` | r3=pb | `OS::Native::Read(r3)` |
| `PBWriteSync` | r3=pb | `OS::Native::Write(r3)` |
| `PBHCreateSync` | r3=pb | `OS::Native::Create(r3, 0xA208)` |
| `PBSetEOFSync` | r3=pb | `OS::Native::SetEOF(r3)` |
| `PBGetCatInfoSync` | r3=pb | `OS::Native::HFSDispatch(r3, 0x0009)` |
| `PBGetFCBInfoSync` | r3=pb | Custom: return FCB info for refNum |
| `FSRead` | r3=refNum, r4=&count, r5=buf | Build param block, call Read |
| `FSWrite` | r3=refNum, r4=&count, r5=buf | Build param block, call Write |
| `FSClose` | r3=refNum | Build param block, call Close |
| `Create` | r3=name, r4=vRefNum, etc. | Build param block, call Create |
| `FSDelete` | r3=name, r4=vRefNum | Build param block, call Delete |
| `GetFInfo` / `HGetFInfo` | r3=pb | `OS::Native::GetFileInfo(r3, trap)` |
| `SetFInfo` / `HSetFInfo` | r3=pb | `OS::Native::SetFileInfo(r3, trap)` |
| `SetEOF` | r3=refNum, r4=eof | Build param block |
| `GetFPos` / `SetFPos` | r3=refNum, ... | Build param block |
| `HGetVol` | r3=pb | `OS::Native::HGetVol(r3)` |
| `HDelete` | r3=pb | `OS::Native::Delete(r3, 0xA209)` |
| `FSMakeFSSpec` | r3=vRefNum, r4=dirID, r5=name, r6=specPtr | Build FSSpec |
| `ResolveAliasFile` | r3=specPtr, r4=resolve, r5=isFolder, r6=wasAliased | `OS::ResolveAliasFile()` |
| `Rename` | r3=name, r4=vRefNum, r5=newName | Build param block |

**Special: PBHOpenSync**

StdCLib opens "stdin", "stdout", "stderr" by name during initialization. We must intercept these:

```cpp
static void wrap_PBHOpenSync() {
    uint32_t pb = PPC::GetGPR(3);
    // Read the filename from the param block
    uint32_t namePtr = memoryReadLong(pb + 18); // ioNamePtr
    std::string name = readPString(namePtr);     // Pascal string

    // Map special names to host fds
    if (name == "stdin" || name == "stdout" || name == "stderr") {
        int hostFd = (name == "stdin") ? 0 : (name == "stdout") ? 1 : 2;
        int16_t refNum = /* fd-to-refNum mapping */;
        memoryWriteWord(refNum, pb + 24);  // ioRefNum
        memoryWriteWord(0, pb + 16);       // ioResult = noErr
        PPC::SetGPR(3, 0);                 // return noErr
        return;
    }

    // Otherwise, pass through to normal Open
    uint16_t err = OS::Native::HFSDispatch(pb, 0x0001); // PBHOpen
    PPC::SetGPR(3, err);
}
```

### Trap Manager

StdCLib calls these to check if traps are available:

| Symbol | Implementation |
|---|---|
| `NGetTrapAddress` | r3=trapNum, r4=tType. Return a trap glue address (any non-zero value indicates the trap exists) |
| `GetToolTrapAddress` | Same, for tool traps |
| `GetOSTrapAddress` | Same, for OS traps |

The important thing is returning non-zero for traps that exist and the unimplemented-trap address for traps that don't. The existing `OS::GetToolTrapAddress` / `OS::GetOSTrapAddress` implementations handle this; we just need to bridge the calling convention.

### Gestalt Manager

```cpp
static void wrap_Gestalt() {
    uint32_t selector = PPC::GetGPR(3);
    uint32_t responsePtr = PPC::GetGPR(4);
    // Use existing Gestalt implementation
    // Set A0=selector for the 68K handler, or implement natively
    cpuSetDReg(0, selector);
    uint16_t err = OS::Gestalt(0xA1AD);
    if (responsePtr) {
        memoryWriteLong(cpuGetAReg(0), responsePtr);
    }
    PPC::SetGPR(3, err);
}
```

### Resource Manager (bridge to `RM::Native`)

| Symbol | Implementation |
|---|---|
| `GetResource` | `RM::Native::GetResource(r3, r4, handle); return handle` |
| `Get1Resource` | Same with Get1Resource variant |
| `SetResLoad` | `RM::Native::SetResLoad(r3)` |
| `ReleaseResource` | `RM::Native::ReleaseResource(r3)` |
| `ResError` | `return RM::Native::ResError()` |
| `CurResFile` | `return RM::Native::CurResFile()` |
| `UseResFile` | `RM::Native::UseResFile(r3)` |
| `OpenResFile` | `RM::Native::OpenResFile(readPString(r3)); return refNum` |
| `CloseResFile` | `RM::Native::CloseResFile(r3)` |
| `HomeResFile` | `return RM::Native::HomeResFile(r3)` |
| `GetResAttrs` | `return RM::Native::GetResAttrs(r3)` |
| `GetResInfo` | `RM::Native::GetResInfo(r3, &id, &type, &name)` — write to r4/r5/r6 ptrs |
| `GetResourceSizeOnDisk` | `return RM::Native::GetResourceSizeOnDisk(r3)` |
| `AddResource` | `RM::Native::AddResource(r3, r4, r5, readPString(r6))` |
| `ChangedResource` | `RM::Native::ChangedResource(r3)` |
| `UpdateResFile` | `RM::Native::UpdateResFile(r3)` |
| `RemoveResource` | `RM::Native::RemoveResource(r3)` |
| `GetIntlResource` | `RM::Native::GetResource('itl0' or 'itl1', r3, handle)` |
| `ReadPartialResource` | Custom implementation using existing RM internals |

### Time Functions

| Symbol | PPC Args | Implementation |
|---|---|---|
| `GetDateTime` | r3=&secs | Write current Mac timestamp to *r3 |
| `ReadDateTime` | r3=&secs | Same |
| `SecondsToDate` | r3=secs, r4=&dateRec | Convert seconds to DateTimeRec |
| `DateToSeconds` | r3=&dateRec, r4=&secs | Convert DateTimeRec to seconds |
| `TickCount` | (none) | Return tick count (~60Hz counter) |

### String Conversions

| Symbol | Implementation |
|---|---|
| `c2pstr` / `C2PStr` | Convert C string at r3 to Pascal string in-place |
| `p2cstr` / `P2CStr` | Convert Pascal string at r3 to C string in-place |

### Process Manager

| Symbol | Implementation |
|---|---|
| `GetCurrentProcess` | Write a fake PSN to *r3 |
| `GetProcessInformation` | Fill ProcessInfoRec at r4 with app info |
| `ExitToShell` | `PPC::Stop()` — halt emulation |

### Mixed Mode Manager

**`NewRoutineDescriptor(procPtr, procInfo, ISA)`:**
For PPC (ISA = kPowerPCISA = 1), just return the procPtr unchanged — it's already a TVector. For 68K (ISA = kM68kISA = 0), we'd need to wrap it, but this shouldn't happen in a PPC-only context.

**`DisposeRoutineDescriptor(rd)`:**
If the descriptor starts with `0xAAFE` (routine descriptor magic), dispose it. Otherwise it's a passthrough TVector — do nothing.

**`CallUniversalProc` — PPC Trampoline:**

This is the most complex wrapper. It's real PPC code allocated in emulated memory via `CFMStubs::AllocateCode()`. When StdCLib calls CallUniversalProc, the trampoline:

1. Saves LR and caller's r2 on the stack
2. Loads the first halfword from r3 (the proc pointer)
3. Compares against `0xAAFE` (routine descriptor magic)
4. **If 68K descriptor**: Branch to an sc stub that handles the 68K dispatch
5. **If PPC TVector**: Shift args (r5→r3, r6→r4, etc.), load code+TOC from TVector, call via `bctrl`
6. Restore r2 and LR, return

The arg shifting is needed because CallUniversalProc's args are `(procPtr, procInfo, arg1, arg2, ...)` but the target function expects `(arg1, arg2, ...)` — so r5→r3, r6→r4, r7→r5, r8→r6, r9→r7, r10→r8.

```cpp
// Trampoline PPC code (approximately 28 instructions):
uint32_t code[] = {
    0x7C0802A6, // mflr    r0
    0x90010008, // stw     r0, 8(r1)       ; save LR
    0x9421FFC0, // stwu    r1, -64(r1)     ; allocate frame
    0x90410038, // stw     r2, 56(r1)      ; save TOC
    0xA0030000, // lhz     r0, 0(r3)       ; load first halfword of procPtr
    0x2C00AAFE, // cmpwi   cr0, r0, -21762 ; compare to 0xAAFE
    0x41820024, // beq     cr0, 68K_path   ; if match, goto 68K handler
    // PPC path: r3=TVector
    0x7C6C1B78, // mr      r12, r3         ; r12 = TVector
    0x7CA32B78, // mr      r3, r5          ; shift args: r5→r3
    0x7CC43378, // mr      r4, r6          ; r6→r4
    0x7CE53B78, // mr      r5, r7          ; r7→r5
    0x7D064378, // mr      r6, r8          ; r8→r6
    0x7D274B78, // mr      r7, r9          ; r9→r7
    0x7D485378, // mr      r8, r10         ; r10→r8
    0x800C0000, // lwz     r0, 0(r12)      ; code addr from TVector
    0x804C0004, // lwz     r2, 4(r12)      ; TOC from TVector
    0x7C0903A6, // mtctr   r0
    0x4E800421, // bctrl                   ; call target
    0x48000014, // b       epilogue        ; skip 68K path
    // 68K path: dispatch via sc stub (4 instructions, patched at runtime)
    0x3D800000, // lis     r12, hi(sc68K_code)  ; PATCHED
    0x618C0000, // ori     r12, r12, lo(sc68K_code) ; PATCHED
    0x7D8903A6, // mtctr   r12
    0x4E800420, // bctr                    ; tail-call to 68K handler
    // Epilogue (PPC return path):
    0x80410038, // lwz     r2, 56(r1)      ; restore TOC
    0x38210040, // addi    r1, r1, 64      ; deallocate frame
    0x80010008, // lwz     r0, 8(r1)       ; load saved LR
    0x7C0803A6, // mtlr    r0
    0x4E800020, // blr
};
```

The 68K path instructions (indices 19-20) are patched at runtime with the address of the sc stub for 68K dispatch.

Register this trampoline with:
```cpp
uint32_t tvec = CFMStubs::AllocateCode(code, count);
CFMStubs::RegisterTVector("InterfaceLib", "CallUniversalProc", tvec);
```

### MathLib Functions

StdCLib imports `str2dec`, `dec2num`, `dec2numl`, `num2decl` from MathLib for printf/scanf decimal conversion. These bridge to the existing SANE library:

| Symbol | Implementation |
|---|---|
| `str2dec` | r3=str, r4=&ix, r5=&decimal, r6=&vp. Parse decimal string using `SANE::str2dec` |
| `dec2num` | r3=&decimal → f1=double. Convert using `SANE::dec2x` |
| `dec2numl` | Same as dec2num (long double == double on PPC 603e) |
| `num2decl` | r3=&decform, f1=value, r4=&decimal. Convert using `SANE::x2dec` |

The PPC decimal struct is 42 bytes: `{int8 sgn, int8 unused, int16 exp, uint8 sig_len, char sig[36], uint8 unused2}`.

### PrivateInterfaceLib

| Symbol | Implementation |
|---|---|
| `GetEmulatorRegister` | Return 0 (we're not under 68K emulation) |
| `SetEmulatorRegister` | No-op |

### Miscellaneous

| Symbol | Implementation |
|---|---|
| `DebugStr` | Read Pascal string from r3, print to stderr |
| `LMGetCurApName` | Return `memoryReadLong(MacOS::CurApName)` |
| `LMGetBootDrive` | Return `memoryReadWord(0x0210)` |
| `FindFolder` | Return fnfErr (-43) |
| `numtostring` | Convert r3 to Pascal string at r4 |
| `GetCursor` | Return 0 (no cursors in CLI) |
| `SetCursor` / `ShowCursor` | No-op |

---

## Debugging and Tracing in This Phase

### Enhanced dispatch tracing for InterfaceLib wrappers

The `--trace-toolbox` flag (from Phase 3) shows stub names and raw register values. For InterfaceLib wrappers, we can improve the trace output by decoding arguments in domain-specific ways. Use a trace helper:

```cpp
static bool Trace = false;  // set from CFMStubs trace flag

// In each wrapper, add trace output when Trace is true:
static void wrap_NewPtr() {
    uint32_t size = PPC::GetGPR(3);
    uint32_t ptr = 0;
    MM::Native::NewPtr(size, false, ptr);
    if (Trace) fprintf(stderr, "  NewPtr(0x%X) -> 0x%08X\n", size, ptr);
    PPC::SetGPR(3, ptr);
}

static void wrap_PBHOpenSync() {
    uint32_t pb = PPC::GetGPR(3);
    std::string name = readPString(memoryReadLong(pb + 18));
    if (Trace) fprintf(stderr, "  PBHOpenSync(\"%s\")\n", name.c_str());
    // ... implementation ...
    if (Trace) fprintf(stderr, "    -> refNum=%d, err=%d\n", refNum, err);
}
```

This is particularly important for:
- **PBHOpenSync**: Shows whether "stdin"/"stdout"/"stderr" opens are being intercepted correctly
- **Gestalt**: Shows which selectors StdCLib queries and what we return
- **NewRoutineDescriptor**: Shows what ISA and procInfo are being used
- **CallUniversalProc**: Shows the target address and whether it takes the PPC or 68K path

### Catching unimplemented stubs

Register a catch-all handler for any unresolved import that logs the call and halts:

```cpp
// During StdCLib load, register a fallback for unresolved imports:
auto resolver = [](const std::string &lib, const std::string &sym, uint8_t cls) -> uint32_t {
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (!addr) {
        // Register a "missing stub" handler that logs and halts
        addr = CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
            fprintf(stderr, "PPC FATAL: unimplemented stub %s::%s called\n",
                    lib.c_str(), sym.c_str());
            fprintf(stderr, "  r3=0x%08X r4=0x%08X r5=0x%08X LR=0x%08X\n",
                    PPC::GetGPR(3), PPC::GetGPR(4), PPC::GetGPR(5), PPC::GetLR());
            PPC::Stop();
        });
    }
    return addr;
};
```

This way, if StdCLib calls a function we haven't implemented, we get a clear error message with register context instead of a silent crash.

---

## Files to Create

### `toolbox/ppc_dispatch.h`

```cpp
#ifndef __mpw_ppc_dispatch_h__
#define __mpw_ppc_dispatch_h__

namespace PPCDispatch {
    // Register all InterfaceLib, MathLib, and PrivateInterfaceLib stubs.
    void RegisterAll();
}

#endif
```

### `toolbox/ppc_dispatch.cpp`

All wrapper functions and the `RegisterAll()` function. Organized by category (Memory Manager, File Manager, etc.). Each wrapper follows the pattern:

```cpp
static void wrap_NewPtr() {
    uint32_t size = PPC::GetGPR(3);
    uint32_t ptr = 0;
    MM::Native::NewPtr(size, false, ptr);
    PPC::SetGPR(3, ptr);
}
```

The `RegisterAll()` function calls `CFMStubs::RegisterStub()` for every wrapper.

---

## Files to Modify

### `toolbox/CMakeLists.txt`

Add `ppc_dispatch.cpp` to the TOOLBOX_LIB source list.

---

## Implementation Steps

1. Write `toolbox/ppc_dispatch.h`
2. Write `toolbox/ppc_dispatch.cpp`:
   a. Helper functions (readPString, readCString, writePString, writeCString)
   b. Memory Manager wrappers (~15 functions)
   c. File Manager wrappers (~25 functions), including PBHOpenSync special case
   d. Trap Manager wrappers (3 functions)
   e. Gestalt wrapper
   f. Resource Manager wrappers (~18 functions)
   g. Time wrappers (4 functions)
   h. String conversion wrappers (2 functions)
   i. Process Manager wrappers (3 functions)
   j. Mixed Mode Manager (NewRoutineDescriptor, DisposeRoutineDescriptor, CallUniversalProc trampoline)
   k. MathLib wrappers (4 functions)
   l. PrivateInterfaceLib wrappers (2 functions)
   m. Miscellaneous wrappers (~10 functions)
   n. `RegisterAll()` — register all of the above
3. Modify `toolbox/CMakeLists.txt`

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```

### Stub registration test

After calling `PPCDispatch::RegisterAll()`, verify all stubs are registered by attempting to resolve known imports:

```cpp
assert(CFMStubs::ResolveImport("InterfaceLib", "NewPtr") != 0);
assert(CFMStubs::ResolveImport("InterfaceLib", "Gestalt") != 0);
assert(CFMStubs::ResolveImport("MathLib", "str2dec") != 0);
```

### StdCLib load test

Use Phase 2's PEF loader to load StdCLib with a resolver that checks CFM stubs first, then registers catch-all handlers for anything missing. Since InterfaceLib/MathLib/PrivateInterfaceLib are stub libraries (no code), we do NOT load their PEFs — our registered stubs ARE the implementation:

```cpp
auto resolver = [](const std::string &lib, const std::string &sym, uint8_t cls) -> uint32_t {
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (!addr) {
        // Register catch-all that logs and halts
        addr = CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
            fprintf(stderr, "PPC FATAL: unimplemented stub %s::%s\n", lib.c_str(), sym.c_str());
            PPC::Stop();
        });
    }
    return addr;
};
PEFLoader::LoadPEFFile(stdclibPath, resolver, stdclibResult);
```

All 66 imports (60 InterfaceLib + 4 MathLib + 2 PrivateInterfaceLib) should resolve against our registered stubs. If we missed any, the catch-all handler will fire at runtime with a clear error message.

### StdCLib init test

If all imports resolve, call StdCLib's `__initialize` entry point:
```cpp
PPCCallFunction(stdclibResult.initPoint);
```

This should complete without crashing. Watch for:
- ECON ioctl calls (logged via trace) — these won't work yet (Phase 5)
- Gestalt calls — should return reasonable values
- Memory allocation — should succeed

If `__initialize` crashes, enable `CFMStubs::SetTrace(true)` to see the sequence of stub calls leading up to the crash. Missing or incorrect stubs will be revealed.

---

## Risk Notes

- **Incomplete stub list**: StdCLib may import functions not listed above. The resolver should log unresolved imports so we can add stubs incrementally.
- **Parameter block layout**: PPC parameter blocks have the same layout as 68K ones (same memory offsets). But some wrappers that were designed for 68K (reading from A0 register) need to be adapted to read from r3.
- **CallUniversalProc trampoline correctness**: The PPC trampoline code must be exact — wrong branch offsets will cause hard-to-debug crashes. Validate each instruction encoding carefully, especially the branch offset for the `beq` to the 68K path.
- **MathLib decimal struct layout**: The struct packing may differ between 68K and PPC. Verify byte offsets match what StdCLib expects.
