# Phase 4: StdCLib InterfaceLib/MathLib Wrappers

**Goal:** Implement the 66 functions that StdCLib imports from InterfaceLib (60), MathLib (4), and PrivateInterfaceLib (2), bridging PPC calling convention to existing `Native::` APIs.

**Depends on:** Phase 1 (PPC register access), Phase 2 (PEF loader), Phase 3 (CFM stub registration).

---

## Overview

As discovered in Phase 3 analysis, InterfaceLib, MathLib, and PrivateInterfaceLib are **stub libraries** — PEFs with no code or data, only export catalogs. We do NOT load their PEFs. Instead, we register a native C++ handler for each function that StdCLib imports. The CFM stub system from Phase 3 allocates a TVector with an `sc` instruction for each handler.

This phase implements **only** the 66 symbols StdCLib actually imports (verified via DumpPEF). Additional wrappers for symbols that other tools import directly from InterfaceLib are deferred to Phase 4b.

The PPC calling convention: r3-r10 are parameters (left to right), r3 is the return value, f1-f13 for FP parameters, f1 for FP return.

---

## Complete Import List (from DumpPEF)

### InterfaceLib (60 symbols)

**Memory Manager** (8 symbols — bridge to `MM::Native`):

| Symbol | PPC Args | Implementation |
|---|---|---|
| `NewPtr` | r3=size | `MM::Native::NewPtr(r3, false, ptr); return ptr` |
| `DisposePtr` | r3=ptr | `MM::Native::DisposePtr(r3)` |
| `GetPtrSize` | r3=ptr | Read via 68K: `cpuSetAReg(0, r3); MM::GetPtrSize(0xA021); return cpuGetDReg(0)` |
| `SetPtrSize` | r3=ptr, r4=size | No-op, return 0 (noErr) |
| `NewHandle` | r3=size | `MM::Native::NewHandle(r3, false, h); return h` |
| `DisposeHandle` | r3=handle | `MM::Native::DisposeHandle(r3)` |
| `HLock` | r3=handle | `MM::Native::HLock(r3); return 0` |
| `HUnlock` | r3=handle | `MM::Native::HUnlock(r3); return 0` |

**Memory Manager — misc** (4 symbols):

| Symbol | Implementation |
|---|---|
| `BlockMove` | r3=src, r4=dst, r5=size. `memmove(memPtr(r4), memPtr(r3), r5)` |
| `FreeMem` | Return large value (available memory) |
| `MemError` | `return memoryReadWord(0x0220)` (MemErr low-memory global) |
| `GetZone` / `SetZone` | Read/write `MacOS::TheZone` low-memory global |

**File Manager — PB calls** (9 symbols — bridge to `OS::Native`):

| Symbol | Implementation |
|---|---|
| `PBHOpenSync` | r3=pb. **Must map "stdin"/"stdout"/"stderr" to host fds.** Otherwise `OS::Native::HFSDispatch(r3, 0x0001)` |
| `PBHOpenRFSync` | r3=pb. `OS::Native::OpenRF(r3, 0xA200)` |
| `PBCloseSync` | r3=pb. `OS::Native::Close(r3)` |
| `PBHCreateSync` | r3=pb. `OS::Native::Create(r3, 0xA208)` |
| `PBSetEOFSync` | r3=pb. `OS::Native::SetEOF(r3)` |
| `PBGetCatInfoSync` | r3=pb. `OS::Native::HFSDispatch(r3, 0x0009)` |
| `PBGetFCBInfoSync` | r3=pb. Custom: return FCB info for refNum |

**File Manager — high-level calls** (13 symbols):

| Symbol | PPC Args | Implementation |
|---|---|---|
| `FSRead` | r3=refNum, r4=&count, r5=buf | Build IOParam on stack, call `OS::Native::Read` |
| `FSWrite` | r3=refNum, r4=&count, r5=buf | Build IOParam on stack, call `OS::Native::Write` |
| `FSClose` | r3=refNum | Build IOParam, call `OS::Native::Close` |
| `Create` | r3=name, r4=vRefNum, ... | Build param block, call `OS::Native::Create` |
| `FSDelete` | r3=name, r4=vRefNum | Build param block, call `OS::Native::Delete` |
| `GetFInfo` / `HGetFInfo` | r3=pb | `OS::Native::GetFileInfo(r3, trap)` |
| `SetFInfo` / `HSetFInfo` | r3=pb | `OS::Native::SetFileInfo(r3, trap)` |
| `SetEOF` | r3=refNum, r4=eof | Build param block |
| `GetFPos` / `SetFPos` | r3=refNum, ... | Build param block |
| `HGetVol` | r3=pb | `OS::Native::HGetVol(r3)` |
| `HDelete` | r3=pb | `OS::Native::Delete(r3, 0xA209)` |
| `FSMakeFSSpec` | r3=vRefNum, r4=dirID, r5=name, r6=specPtr | Build FSSpec |
| `ResolveAliasFile` | r3=specPtr, r4=resolve, r5=isFolder, r6=wasAliased | Delegate |
| `Rename` | r3=name, r4=vRefNum, r5=newName | Build param block |

**Trap Manager** (1 symbol):

| Symbol | Implementation |
|---|---|
| `NGetTrapAddress` | r3=trapNum, r4=tType. Return trap glue address (non-zero = exists) |

**Gestalt Manager** (1 symbol):

| Symbol | Implementation |
|---|---|
| `Gestalt` | r3=selector, r4=&response. Bridge to existing `OS::Gestalt` |

**Resource Manager** (3 symbols):

| Symbol | Implementation |
|---|---|
| `ReleaseResource` | `RM::Native::ReleaseResource(r3)` |
| `ResError` | `return RM::Native::ResError()` |
| `CurResFile` | `return RM::Native::CurResFile()` |

**Time** (3 symbols):

| Symbol | Implementation |
|---|---|
| `GetDateTime` | r3=&secs. Write Mac timestamp |
| `SecondsToDate` | r3=secs, r4=&dateRec |
| `DateToSeconds` | r3=&dateRec, r4=&secs |
| `TickCount` | Return tick count |

**String Conversions** (4 symbols):

| Symbol | Implementation |
|---|---|
| `c2pstr` / `C2PStr` | Convert C string at r3 to Pascal in-place |
| `p2cstr` / `P2CStr` | Convert Pascal string at r3 to C in-place |

**Process Manager** (3 symbols):

| Symbol | Implementation |
|---|---|
| `GetCurrentProcess` | r3=&psn. Write fake PSN |
| `GetProcessInformation` | r3=&psn, r4=&infoRec. Fill with app info |
| `ExitToShell` | `PPC::Stop()` |

**Mixed Mode Manager** (3 symbols):

| Symbol | Implementation |
|---|---|
| `NewRoutineDescriptor` | r3=procPtr, r4=procInfo, r5=ISA. For PPC, return procPtr unchanged |
| `DisposeRoutineDescriptor` | r3=rd. Dispose if 0xAAFE magic, else no-op |
| `CallUniversalProc` | PPC trampoline (see below) |

**Internationalization** (1 symbol):

| Symbol | Implementation |
|---|---|
| `GetIntlResource` | r3=id. `RM::Native::GetResource('itl0'/'itl1', r3, handle)` |

**Misc** (3 symbols):

| Symbol | Implementation |
|---|---|
| `DebugStr` | r3=pstring. Print to stderr |
| `LMGetCurApName` | Return pointer to `MacOS::CurApName` |
| `GetNodeAddress` | Return 0 (no network) |

### MathLib (4 symbols)

| Symbol | Implementation |
|---|---|
| `str2dec` | r3=str, r4=&ix, r5=&decimal, r6=&vp. Bridge to `SANE::str2dec` |
| `dec2num` | r3=&decimal → f1=double. Bridge to `SANE::dec2x` |
| `dec2numl` | Same as dec2num (long double == double on PPC) |
| `num2decl` | r3=&decform, f1=value, r4=&decimal. Bridge to `SANE::x2dec` |

### PrivateInterfaceLib (2 symbols)

| Symbol | Implementation |
|---|---|
| `GetEmulatorRegister` | Return 0 (not under 68K emulation) |
| `SetEmulatorRegister` | No-op |

---

## CallUniversalProc Trampoline

This is the most complex wrapper — real PPC code allocated in emulated memory via `CFMStubs::AllocateCode()`. When StdCLib calls CallUniversalProc, the trampoline:

1. Saves LR and caller's r2 on the stack
2. Loads the first halfword from r3 (the proc pointer)
3. Compares against `0xAAFE` (routine descriptor magic)
4. **If 68K descriptor**: Branch to an sc stub that handles the 68K dispatch
5. **If PPC TVector**: Shift args (r5→r3, r6→r4, etc.), load code+TOC from TVector, call via `bctrl`
6. Restore r2 and LR, return

The arg shifting is needed because CallUniversalProc's args are `(procPtr, procInfo, arg1, arg2, ...)` but the target function expects `(arg1, arg2, ...)`.

```cpp
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
    // 68K path (4 instructions, patched at runtime with sc68K code addr)
    0x3D800000, // lis     r12, hi(sc68K_code)  ; PATCHED
    0x618C0000, // ori     r12, r12, lo(sc68K_code) ; PATCHED
    0x7D8903A6, // mtctr   r12
    0x4E800420, // bctr                    ; tail-call to 68K handler
    // Epilogue:
    0x80410038, // lwz     r2, 56(r1)      ; restore TOC
    0x38210040, // addi    r1, r1, 64      ; deallocate frame
    0x80010008, // lwz     r0, 8(r1)       ; load saved LR
    0x7C0803A6, // mtlr    r0
    0x4E800020, // blr
};
```

---

## Debugging and Tracing

### Catch-all for unimplemented stubs

The resolver used when loading StdCLib registers a catch-all handler for any import not pre-registered. This logs the stub name and register context, then halts:

```cpp
auto resolver = [](const std::string &lib, const std::string &sym, uint8_t cls) -> uint32_t {
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (!addr) {
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

### `--trace-toolbox` enhanced output

Wrappers should include domain-specific trace output when `--trace-toolbox` is active, particularly for PBHOpenSync (filename), Gestalt (selector), NewRoutineDescriptor (ISA), and CallUniversalProc (PPC vs 68K path).

---

## Files to Create

### `toolbox/ppc_dispatch.h`

```cpp
#ifndef __mpw_ppc_dispatch_h__
#define __mpw_ppc_dispatch_h__

namespace PPCDispatch {
    // Register the 66 stubs that StdCLib imports.
    void RegisterStdCLibImports();
}

#endif
```

### `toolbox/ppc_dispatch.cpp`

All 66 wrapper functions and the `RegisterStdCLibImports()` function. Organized by category.

---

## Files to Modify

### `toolbox/CMakeLists.txt`

Add `ppc_dispatch.cpp` to the TOOLBOX_LIB source list.

---

## Implementation Steps

1. Write `toolbox/ppc_dispatch.h`
2. Write `toolbox/ppc_dispatch.cpp`:
   a. Helper functions (readPString, readCString, writePString, writeCString)
   b. Memory Manager wrappers (12 symbols)
   c. File Manager wrappers (22 symbols), including PBHOpenSync special case
   d. Trap Manager wrapper (1 symbol)
   e. Gestalt wrapper (1 symbol)
   f. Resource Manager wrappers (3 symbols)
   g. Time wrappers (4 symbols)
   h. String conversion wrappers (4 symbols)
   i. Process Manager wrappers (3 symbols)
   j. Mixed Mode Manager (3 symbols, including CallUniversalProc trampoline)
   k. MathLib wrappers (4 symbols)
   l. PrivateInterfaceLib wrappers (2 symbols)
   m. Misc wrappers (4 symbols)
   n. `RegisterStdCLibImports()` — register all 66 stubs
3. Modify `toolbox/CMakeLists.txt`

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```

### Stub registration test

After calling `PPCDispatch::RegisterStdCLibImports()`, verify all 66 StdCLib imports resolve:

```cpp
// Load StdCLib with the CFM resolver — 0 catch-alls expected
uint32_t catchAll = 0;
auto resolver = [&](const std::string &lib, const std::string &sym, uint8_t cls) -> uint32_t {
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (!addr) { catchAll++; /* register catch-all */ }
    return addr;
};
PEFLoader::LoadPEFFile(stdclibPath, resolver, result);
assert(catchAll == 0); // all 66 should be pre-registered
```

### StdCLib init test

Call StdCLib's `__initialize` entry point. This should complete without crashing. Watch for ECON ioctl calls (won't work yet — Phase 5) and Gestalt/NewPtr calls (should work).

---

## Risk Notes

- **CallUniversalProc branch offsets**: The PPC trampoline code must have exact branch offsets. Off-by-4 errors cause hard-to-debug crashes. Validate each instruction encoding.
- **MathLib decimal struct layout**: The 42-byte struct must match what StdCLib expects. Verify byte offsets.
- **PBHOpenSync stdin/stdout/stderr**: The filename matching must handle Pascal strings correctly.
