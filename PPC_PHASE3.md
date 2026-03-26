# Phase 3: CFM Stub System

**Goal:** Provide a mechanism to register native C++ handlers for imported shared library symbols, using the PPC `sc` instruction for dispatch.

**Depends on:** Phase 1 (PPC CPU, for `sc` handler and memory access), Phase 2 (PEF loader, for import resolution callback).

---

## Overview

### Stub libraries vs real libraries

Inspection of the actual MPW shared libraries with `DumpPEF` reveals two distinct kinds:

**Stub libraries (InterfaceLib, MathLib, PrivateInterfaceLib):** These contain **no code and no data** — only a loader section with an export table. Every export has `address=0` and `sectionNumber=-2` (absolute/external). On a real Mac, CFM would resolve these against the Toolbox ROM. In our emulator, **the CFM stub system IS the implementation of these libraries.** We do not load their PEFs at all.

- InterfaceLib: 2489 TVector exports, 0 loadable sections, 0 imports
- MathLib: 177 TVector exports, 0 loadable sections, 0 imports
- PrivateInterfaceLib: similar pattern

**Real libraries (StdCLib):** These contain actual PPC code and data sections with real exports at real addresses. We load these via the PEF loader (Phase 2) and run their native PPC code. Their imports from stub libraries are resolved against our CFM stubs.

### Two resolution paths

When loading a PEF that imports symbols:

1. **Import from a stub library** (InterfaceLib, MathLib, PrivateInterfaceLib): Resolved by `CFMStubs::ResolveImport()` → returns TVector address of our native C++ handler.
2. **Import from a real library** (StdCLib): Resolved by `PEFLoader::FindExport()` on the loaded library → returns the real TVector address in emulated memory. These are registered via `CFMStubs::RegisterTVector()` so the resolver has a single lookup point.

### Stub Execution Flow

When PPC code calls through one of our stub TVectors:

1. PPC code calls through a TVector: loads `{code_addr, toc}`, sets r2=toc, branches to code_addr
2. The stub code at code_addr is: `li r11, <index>; sc`
3. The `sc` instruction triggers the interrupt hook registered in Phase 1
4. The hook dispatches to `CFMStubs::Dispatch()`, which reads r11 to get the stub index and calls the handler
5. The handler reads PPC registers (r3-r10 for args), does its work, writes results (r3 for return)
6. The hook sets PC = LR (return to the caller, since our Phase 1 sc handler returns via LR)

### Why `li r11, <index>` before `sc`?

We need a way to identify which stub was called. Using r11 (a volatile register in the PPC ABI) to pass the stub index is clean — r11 is callee-trashed and not used for parameter passing.

---

## Memory Layout

Allocate a single contiguous block for all stubs:

```
Code region (12 bytes per stub):
  stub[0]: li r11, 0       (0x39600000)     base + 0
           sc              (0x44000002)     base + 4
           blr             (0x4E800020)     base + 8
  stub[1]: li r11, 1       (0x39600001)     base + 12
           sc              (0x44000002)     base + 16
           blr             (0x4E800020)     base + 20
  ...

TVector region (8 bytes per stub):
  tvec[0]: {base + 0, 0}                   tvecBase + 0
  tvec[1]: {base + 12, 0}                  tvecBase + 8
  ...
```

The TVector's TOC field is 0 because our native stubs don't use TOC. The PPC calling convention will have already saved the caller's TOC.

Additionally, the system supports `AllocateCode()` for injecting arbitrary PPC code into emulated memory (used by the CallUniversalProc trampoline in Phase 4).

---

## Files to Create

### `toolbox/cfm_stubs.h`

```cpp
#ifndef __mpw_cfm_stubs_h__
#define __mpw_cfm_stubs_h__

#include <cstdint>
#include <string>
#include <functional>

namespace CFMStubs {
    // Native C++ function called when a stub is invoked.
    // Should read parameters from PPC::GetGPR(3..10) and write
    // results to PPC::SetGPR(3) (or FPR for float returns).
    using Handler = std::function<void()>;

    // Initialize the stub system. Allocates emulated memory for
    // up to kMaxStubs (4096) stub entries.
    void Init();

    // Register a native handler for a library::symbol pair.
    // Returns the TVector address in emulated memory.
    // If already registered, returns the existing TVector address.
    uint32_t RegisterStub(const std::string &library,
                          const std::string &symbol,
                          Handler handler);

    // Look up a previously registered stub.
    // Returns the TVector address, or 0 if not found.
    uint32_t ResolveImport(const std::string &library,
                           const std::string &symbol);

    // Called from the PPC sc handler. Reads r11 to determine which
    // stub was invoked and calls the corresponding handler.
    void Dispatch();

    // Enable/disable dispatch tracing (prints stub name on each call).
    void SetTrace(bool trace);

    // Register an existing TVector address (e.g., an export from a
    // loaded PEF) as the resolution for a library::symbol import.
    // Used to register StdCLib exports for tool import resolution.
    void RegisterTVector(const std::string &library,
                         const std::string &symbol,
                         uint32_t tvecAddr);

    // Allocate a block of PPC code in the stub code region and create
    // a TVector for it. Returns the TVector address.
    // Used for the CallUniversalProc trampoline and similar native
    // PPC code that must exist in emulated memory.
    uint32_t AllocateCode(const uint32_t *instructions,
                          uint32_t count);
}

#endif
```

### `toolbox/cfm_stubs.cpp`

Implementation details:

**Data structures:**
```cpp
struct StubEntry {
    std::string library;
    std::string symbol;
    std::string fullName;   // "library::symbol"
    Handler handler;        // nullptr for TVector-only entries
    uint32_t tvecAddr;      // TVector address in emulated memory
    uint32_t codeAddr;      // code address (for sc stubs)
};

static std::vector<StubEntry> stubs;
static std::unordered_map<std::string, uint32_t> stubMap;  // fullName → index
static uint32_t stubMemBase = 0;    // base of allocated memory
static uint32_t nextCodeOffset = 0;
static uint32_t nextTVecOffset = 0;
```

**Init():**
- Allocate `kMaxStubs * (12 + 8)` bytes via `MM::Native::NewPtr`
- Code region: first `kMaxStubs * 12` bytes
- TVector region: next `kMaxStubs * 8` bytes

**RegisterStub():**
1. Check `stubMap` for existing registration; return if found
2. Allocate next code slot: write `li r11,<index>; sc; blr` (3 instructions, 12 bytes)
3. Allocate next TVector slot: write `{codeAddr, 0}` (8 bytes)
4. Store entry in `stubs` vector, index in `stubMap`
5. Return TVector address

**Dispatch():**
1. Read r11 via `PPC::GetGPR(11)`
2. Bounds-check against `stubs.size()`
3. If trace enabled, print stub name and key register values to stderr
4. Call `stubs[index].handler()`
5. If trace enabled, print return value (r3)
6. If handler is null, print error and call `PPC::Stop()`

### Dispatch Tracing (Critical Debugging Feature)

This is the PPC equivalent of 68K A-line trap tracing. It reuses the existing `--trace-toolbox` flag — when running a PPC tool, `--trace-toolbox` enables CFM stub dispatch tracing instead of A-line trap tracing.

When enabled, every stub dispatch prints:
```
  sc> InterfaceLib::NewPtr(size=0x00001000) -> 0x00120000
  sc> InterfaceLib::Gestalt(sel='sysv', resp=0x00131000) -> 0
  sc> ECON::ioctl(entry=0x00140020, cmd=0x6602, arg=0x00000000) -> 0
```

The entry format shows stub name, decoded arguments (where practical), and return value. This is essential for understanding what StdCLib does during init and for catching incorrect argument passing or return values.

For stubs where argument decoding is impractical, fall back to raw register values:
```
  sc> InterfaceLib::GetProcessInformation(r3=0x00140100, r4=0x00140200) -> 0
```

Wire the flag in `loader.cpp` (in Phase 6 when the PPC execution path is added):
```cpp
if (Flags.traceToolBox) CFMStubs::SetTrace(true);
```

**ResolveImport():**
- Look up `"library::symbol"` in `stubMap`, return TVector address or 0

**RegisterTVector():**
- Add an entry to `stubs`/`stubMap` with `handler = nullptr` and the provided `tvecAddr`
- This is used to register StdCLib's real exports so that the tool's PEF loader can resolve imports against them

**AllocateCode():**
1. Write the instruction array into the next available code region slots
2. Create a TVector pointing to the code
3. Return the TVector address

---

## Files to Modify

### `toolbox/CMakeLists.txt`

Add `cfm_stubs.cpp` to the TOOLBOX_LIB source list.

### `cpu/ppc/ppc.cpp`

Wire the sc handler to call `CFMStubs::Dispatch()`. This is done in the loader (Phase 6) via `PPC::SetSCHandler(CFMStubs::Dispatch)`, so no changes to ppc.cpp are needed here — the connection happens at the call site.

---

## Implementation Steps

1. Write `toolbox/cfm_stubs.h`
2. Write `toolbox/cfm_stubs.cpp`:
   a. `Init()` — allocate stub memory
   b. `RegisterStub()` — write code + TVector, store handler
   c. `ResolveImport()` — lookup
   d. `Dispatch()` — read r11, call handler
   e. `RegisterTVector()` — register pre-existing TVectors
   f. `AllocateCode()` — write arbitrary PPC code, create TVector
   g. `SetTrace()` — toggle tracing
3. Modify `toolbox/CMakeLists.txt`

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```

### Integration test with Phase 1

Extend the Phase 1 test to use CFM stubs:

1. Initialize PPC and CFM stubs
2. Set `PPC::SetSCHandler(CFMStubs::Dispatch)`
3. Register a test stub:
   ```cpp
   CFMStubs::RegisterStub("TestLib", "add", []() {
       uint32_t a = PPC::GetGPR(3);
       uint32_t b = PPC::GetGPR(4);
       PPC::SetGPR(3, a + b);
   });
   ```
4. Get the TVector address: `uint32_t tvec = CFMStubs::ResolveImport("TestLib", "add")`
5. Write PPC code that calls through the TVector:
   ```
   li    r3, 10          # first arg
   li    r4, 20          # second arg
   lis   r12, (tvec>>16) # load TVector address
   ori   r12, r12, (tvec&0xFFFF)
   lwz   r0, 0(r12)      # load code address from TVector
   mtctr r0
   bctrl                  # call through TVector
   # r3 should now be 30
   sc                     # signal to our test harness
   blr                    # return
   ```
6. Set LR=0, execute
7. In the final `sc` handler, verify r3 == 30

### Trace test

Enable `CFMStubs::SetTrace(true)`, register and call a stub, verify the stub name is printed to stderr.

### Integration test with Phase 2

Use the PEF loader to load a PEF file with a resolver that uses `CFMStubs::ResolveImport()`:

```cpp
auto resolver = [](const std::string &lib, const std::string &sym, uint8_t cls) {
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (!addr) {
        fprintf(stderr, "  UNRESOLVED: %s::%s\n", lib.c_str(), sym.c_str());
    }
    return addr;
};
```

Register a few dummy stubs for common InterfaceLib functions and verify the PEF loads without relocation errors.

---

## Risk Notes

- **r11 limit**: `li r11, <index>` uses a 16-bit immediate, limiting to 65535 stubs. With kMaxStubs=4096, this is not an issue. But if we ever need >65535, we'd need `lis + ori`.
- **TVector TOC field**: We set the TOC field to 0. When PPC code calls through a TVector, it sets r2 = TVector[1]. Since our stubs don't use r2, this is fine. But the caller's r2 is lost. The PPC ABI requires the caller to save/restore r2 around indirect calls, so this is correct by convention.
- **Thread safety**: Not needed — the emulator is single-threaded.
