# Plan: PPC MPW Tool Support — Hello World

## Context

We want to run PowerPC MPW tools in the emulator — starting with a minimal "Hello, world!" program that uses `fprintf(stdout, ...)`. This eliminates the need for a full Macintosh emulator when running unit tests for new projects.

Two previous branches (`ppc`, `ppc-dead-end`) explored this with ~10K lines of code. We are **starting fresh on master**, using those branches only as reference for what worked and what didn't. Key learnings from prior work:

- **PEF loader** works (needs TOC base fix: read from init TVector, not data section start)
- **`sc` instruction** is an effective mechanism for intercepting imported library calls
- **Real StdCLib PEF** must be loaded and run natively — no intercepting stdlib functions
- **ECON device handlers** are how StdCLib does I/O under the MPW shell
- **MPGM info+0x24** must be set up for `_RTExit`'s longjmp exit chain
- **FIOINTERACTIVE ioctl** must return correctly so stdout gets line-buffered

The existing `Native::` APIs on master (`MM::Native`, `OS::Native`, `RM::Native`) provide clean C++ interfaces to OS/Toolbox functions without 68K register dependencies — ideal for PPC wrappers.

**StdCLib and other shared libraries are available at `~/mpw`.**

---

## Phase 1: PPC CPU Core (Unicorn Engine)

**Goal:** Integrate Unicorn Engine as the PowerPC execution engine.

Unicorn Engine wraps QEMU's CPU emulation in a clean C API. It's GPL v2, supports PPC 32-bit big-endian, and provides `uc_mem_map_ptr()` to share memory with our existing 68K emulator.

### Work
- Add Unicorn as a dependency (find via CMake `find_package` or `pkg-config`)
- Create a thin wrapper in `cpu/ppc/` that provides our project's PPC interface
- Initialize: `uc_open(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, &uc)`
- Map the existing emulated memory buffer via `uc_mem_map_ptr()` — same address space as 68K. **Note:** Unicorn requires 4KB-aligned addresses and sizes; the memory allocation in `loader.cpp` may need to use `aligned_alloc` or `mmap` instead of `new`.
- Register `UC_HOOK_INTR` to catch `sc` instructions. In the hook callback:
  1. Read SRR0 (return address, set by PPC to instruction after `sc`)
  2. Dispatch to the registered CFM stub handler
  3. Set PC = SRR0 to resume after the system call
- Expose a clean C++ interface that hides Unicorn details

### Interface (`cpu/ppc/ppc.h`)
```cpp
namespace PPC {
    using SCHandler = std::function<void()>;

    void Init(uint8_t *memory, uint32_t memorySize);
    void Shutdown();
    void Execute(uint32_t startPC, uint32_t startTOC);  // run until halt
    void Stop();                                         // halt from within handler

    uint32_t GetGPR(int r);
    void SetGPR(int r, uint32_t val);
    double GetFPR(int r);
    void SetFPR(int r, double val);
    uint32_t GetPC();
    void SetPC(uint32_t pc);
    uint32_t GetLR();
    void SetLR(uint32_t lr);

    void SetSCHandler(SCHandler handler);
}
```

### Files
- `cpu/ppc/ppc.h` — interface
- `cpu/ppc/ppc.cpp` — Unicorn wrapper implementation
- `cpu/ppc/CMakeLists.txt` — build config, find Unicorn

### Verification
- Write a small test: load a few PPC instructions (e.g., `li r3, 42; sc`) into memory, execute, verify r3==42 and the sc handler fires.

---

## Phase 2: PEF Loader

**Goal:** Parse and load PEF (Preferred Executable Format) containers into emulated memory.

### Work
- Implement PEF container header parsing (`'Joy!' 'peff'` magic, architecture `'pwpc'`)
- Load sections into emulated memory (allocate via `MM::Native::NewPtr`):
  - **Code** (kind 0): read-only, copy directly
  - **Unpacked data** (kind 1): copy directly
  - **Pattern-initialized data** (kind 2): decompress PEF pidata encoding
  - **Zero-initialized** (kind 4): allocate zeroed memory
- Implement pidata decompression (PEF's run-length encoding: opcodes for defaultByte, blockCopy, repeatBlock, interleaveRepeat, etc.)
- Parse loader section: imported libraries table, imported symbols table, relocation headers, exported symbol hash table
- Implement PEF relocation engine (process relocation opcodes: `RelocBySectDWithLength`, `RelocBySectC`, `RelocTVector`, `RelocImportRun`, `RelocSmByImport`, `RelocSmSetSectD`, etc.)
- Import resolution via callback: for each imported symbol, call a `resolver(library, name, class) → address` function
- **TOC base fix**: When a fragment has an init entry but no main entry, read TOC from the init TVector's second word (not the data section start address)
- Return: entry point address, init entry point, TOC base, section info, export table

### Interface (`toolbox/pef_loader.h`)
```cpp
namespace PEFLoader {
    struct LoadResult {
        uint32_t entryPoint;    // main entry (or 0)
        uint32_t initPoint;     // init entry (or 0)
        uint32_t tocBase;       // r2 value
        std::vector<SectionInfo> sections;
        std::vector<ExportedSymbolInfo> exports;
    };

    using ImportResolver = std::function<uint32_t(
        const std::string &library, const std::string &symbol, uint8_t symClass)>;

    bool IsPEF(const uint8_t *data, size_t size);
    bool LoadPEF(const uint8_t *data, size_t size, ImportResolver resolver, LoadResult &result);
    bool LoadPEFFile(const std::string &path, ImportResolver resolver, LoadResult &result);
    uint32_t FindExport(const LoadResult &result, const std::string &name);
}
```

### Key structures (`toolbox/pef.h`)
- PEF container header (40 bytes)
- PEF section header (28 bytes per section)
- PEF loader header
- Imported library descriptor
- Imported symbol entry
- Exported symbol hash table + chain
- Relocation instruction opcodes

### Files
- `toolbox/pef.h` — PEF container structure definitions
- `toolbox/pef_loader.h` — loader interface
- `toolbox/pef_loader.cpp` — implementation

### Verification
- Load the Hello PEF tool and verify: sections loaded at correct sizes, imports enumerated, exports found, TOC base correct. Log all imports to identify what InterfaceLib/StdCLib symbols are needed.

---

## Phase 3: CFM Stub System

**Goal:** Provide a mechanism to register native C++ handlers for imported shared library symbols.

### Key finding: InterfaceLib/MathLib are stub libraries

DumpPEF analysis reveals that InterfaceLib, MathLib, and PrivateInterfaceLib contain **no code and no data** — only a loader section with an export catalog (all exports have address=0, sectionNumber=-2). On a real Mac, CFM resolves these against the Toolbox ROM. In our emulator, the CFM stub system IS the implementation of these libraries. We do NOT load their PEFs.

### Design
When the PEF loader resolves an import (e.g., `InterfaceLib::NewPtr`), the CFM stub system:
1. Allocates a **TVector** in emulated memory: `{code_addr, toc}` (8 bytes)
2. `code_addr` points to an `sc` instruction (`0x44000002`) followed by `blr` (`0x4E800020`)
3. Each stub gets a unique index; when `sc` fires, the Unicorn interrupt hook reads PC to determine which stub was called
4. The `sc` handler invokes the registered C++ function, which reads/writes PPC registers via `PPC::GetGPR()`/`PPC::SetGPR()`

### Stub Memory Layout
Allocate a contiguous block for all stubs:
```
stub[0]: sc (0x44000002)  at base+0
         blr (0x4E800020) at base+4
stub[1]: sc (0x44000002)  at base+8
         blr (0x4E800020) at base+12
...
```
The `sc` handler computes the stub index from `(sc_address - base) / 8`.

### Interface (`toolbox/cfm_stubs.h`)
```cpp
namespace CFMStubs {
    using Handler = std::function<void()>;

    void Init();                                               // allocate stub table
    void RegisterStub(const std::string &lib,
                      const std::string &sym, Handler handler);
    uint32_t Resolve(const std::string &lib,
                     const std::string &sym, uint8_t symClass); // → TVector address
    void Dispatch(uint32_t pc);                                // called from sc hook
}
```

### Files
- `toolbox/cfm_stubs.h`
- `toolbox/cfm_stubs.cpp`

### Verification
- Register a test stub, resolve it, verify the TVector points to valid `sc; blr` instructions. Execute a `bl` to the stub's code address, verify the handler fires and execution returns.

---

## Phase 4: StdCLib InterfaceLib/MathLib Wrappers

**Goal:** Implement exactly the 66 functions that StdCLib imports from InterfaceLib (60), MathLib (4), and PrivateInterfaceLib (2).

StdCLib's imports were verified via DumpPEF. We do NOT load InterfaceLib/MathLib PEFs (they are stub libraries with no code). Our CFM stubs ARE the implementation.

Key wrappers: Memory Manager (NewPtr, DisposePtr, NewHandle, etc.), File Manager (PBHOpenSync with stdin/stdout/stderr mapping, FSRead, FSWrite, etc.), Gestalt, NGetTrapAddress, Mixed Mode Manager (NewRoutineDescriptor, CallUniversalProc trampoline), MathLib decimal conversion (str2dec, dec2num, num2decl), string conversions (c2pstr, p2cstr), process manager, time functions.

### Files
- `toolbox/ppc_dispatch.h`
- `toolbox/ppc_dispatch.cpp`

### Verification
- Load StdCLib with CFM resolver — all 66 imports resolve with 0 catch-alls.
- Call StdCLib's `__initialize` — should complete without crashing.

---

## Phase 4b: Additional InterfaceLib Wrappers for Tools

**Goal:** Implement additional InterfaceLib wrappers that PPC tools import directly (beyond what StdCLib needs). Demand-driven — add as tools require them.

Likely symbols: NewPtrClear, NewHandleClear, GetHandleSize, BlockMoveData, Resource Manager extras (GetResource, OpenResFile, etc.), PBReadSync, PBWriteSync, FindFolder, etc.

---

## Phase 5: PPC Tool Loading and Execution

**Goal:** Wire everything together in `bin/loader.cpp` to load and run a PPC MPW tool. StdCLib init will crash at ECON device calls (Phase 6), but the integration harness is in place.

See PPC_PHASE5.md for details.

---

## Phase 6: MPW Environment for PPC

**Goal:** Set up the runtime data structures that StdCLib expects when running under the MPW shell.

### MPGM Info Block
The existing `MPW::Init()` creates the MPGM block. For PPC, we either extend it or create a `MPW::InitPPC()` variant with additional fields:

```
MPGM header (at low memory 0x0316):
  +0x00: 'MPGM' magic (4 bytes)
  +0x04: pointer to info block (4 bytes)
Info block:
  +0x00: 'SH' magic (0x5348)
  +0x02: argc (uint32)
  +0x06: argv pointer (uint32)
  +0x0A: envp pointer (uint32)
  +0x0E: exit code (uint32)
  +0x1A: fd table size (uint16, 0x190)
  +0x1C: io table pointer (uint32)
  +0x20: device table pointer (uint32)
  +0x24: exit chain pointer (uint32)     ← NEW
  +0x28: startup entry list (uint32)     ← NEW (NULL initially)
```

### ECON Device Table
For PPC, device table handler addresses must be PPC-callable TVectors (pointing to sc stubs), not inline 68K traps. Create sc stubs for each ECON handler:

**ECON handlers:**
- **faccess**: return 0 (success)
- **close**: no-op for console fds, return 0
- **read**: read from host fd (using cookie→fd mapping), write data to emulated buffer
- **write**: read data from emulated buffer, write to host fd, CR→LF conversion for text mode
- **ioctl**: dispatch by command:
  - `FIOINTERACTIVE (0x6602)`: return 0 (interactive) — **critical for line-buffering**
  - `FIOBUFSIZE (0x6603)`: return 0, write 2048 to *arg
  - `FIODUPFD (0x6601)`: register cookie→fd mapping
  - `FIOREFNUM (0x6605)`: return -1 (no refNum for console)

**FSYS device handlers**: Point to sc stubs that bridge to the existing F-trap handlers (`ftrap_read`, `ftrap_write`, etc.).

### Cookie Structure
Cookies are **pointers** to per-fd state, not bare fd numbers:
```
cookie (allocated per fd):
  +0x00: uint8_t mode (0x01=read, 0x02=write)
  +0x01-0x0B: padding/reserved
  +0x0C: uint8_t connected (1 = has device connection)
```

We maintain a host-side `map<uint32_t, int>` mapping cookie addresses to host fd numbers. Populated during FIODUPFD.

### IO Table
Three 20-byte ioEntry records (stdin, stdout, stderr):
```
ioEntry:
  +0x00: uint16 flags (0x0001=read, 0x0002=write)
  +0x02: uint16 error
  +0x04: uint32 device pointer → ECON device table entry
  +0x08: uint32 cookie → pointer to cookie struct
  +0x0C: uint32 count
  +0x10: uint32 buffer
```

### Exit Chain (info+0x24)
`_RTExit` walks: `p1 = *(info+0x24)`, `p2 = *(p1+0x16)`, `jmpbuf = *p2`, `longjmp(jmpbuf, 1)`.

After loading StdCLib PEF:
1. Look up `__target_for_exit` from StdCLib's export table
2. Allocate chain node (0x1A+ bytes): set `*(node+0x16) = addr_of_ptr_slot`
3. Allocate ptr_slot (4 bytes): set `*ptr_slot = __target_for_exit`
4. Set `info+0x24 = node`

**Important**: This chain layout is based on the findings document and needs runtime verification. If the chain walk is different, we'll adjust in Phase 7.

### Startup Entry List (info+0x28)
StdCLib's init helper walks this for 'getv', 'setv', 'syst', 'strt' entries. Start with NULL. If StdCLib init fails without it, add entries backed by sc stubs for environment variable access.

### Files
- `mpw/mpw.cpp` — extend `Init()` or add `InitPPC()` for device table, cookies, exit chain
- `mpw/mpw.h` — new declarations

### Verification
- Trace StdCLib init to verify ECON ioctls return correct values, stdout FILE gets line-buffer flag set.

---

See PPC_PHASE6.md for details.

### Verification
- `mpw --ppc tools/Hello` prints "Hello, world!" to stdout, exits with status 0.

---

## Phase 7: Debugging and Iteration

The exit chain and line-buffering details are partially speculative. This phase covers diagnosing and fixing what doesn't work.

### Diagnostic Tools
All PPC tracing reuses existing CLI flags — no new flags needed:
- `--trace-cpu`: PPC instruction trace (via `UC_HOOK_CODE`)
- `--trace-toolbox`: CFM stub dispatch + PEF loader logging
- `--trace-mpw`: ECON device handler calls
- `--debug`: Interactive debugger with PPC register display
- Unresolved imports are logged automatically during PEF loading

### Likely Issues to Debug
1. **Exit chain layout wrong**: Trace `_RTExit` instruction-by-instruction to see exact pointer dereferences. Adjust info+0x24 setup.
2. **stdout not line-buffered**: Trace StdCLib's FILE init from FIOINTERACTIVE check to flag setting. Cookie structure bytes may need adjustment.
3. **Missing InterfaceLib stubs**: StdCLib may import functions we didn't implement. CFM resolver should log warnings; add stubs as needed.
4. **CallUniversalProc edge cases**: The trampoline may need to handle additional dispatch patterns.
5. **Unicorn sc handling**: If SRR0/PC management after `sc` doesn't work as expected, adjust the interrupt hook to manually set PC.

---

## Summary of New Files

| File | Purpose |
|------|---------|
| `cpu/ppc/ppc.h` | PPC CPU interface (wraps Unicorn) |
| `cpu/ppc/ppc.cpp` | Unicorn Engine wrapper implementation |
| `cpu/ppc/CMakeLists.txt` | Build config, find Unicorn dependency |
| `toolbox/pef.h` | PEF container structure definitions |
| `toolbox/pef_loader.h` | PEF loader interface |
| `toolbox/pef_loader.cpp` | PEF loading, relocation, import resolution |
| `toolbox/cfm_stubs.h` | sc-based stub dispatch system interface |
| `toolbox/cfm_stubs.cpp` | Stub dispatch implementation |
| `toolbox/ppc_dispatch.h` | InterfaceLib wrappers + ECON handlers interface |
| `toolbox/ppc_dispatch.cpp` | InterfaceLib/ECON handler implementations |

## Modified Files

| File | Changes |
|------|---------|
| `cpu/CMakeLists.txt` | Add `ppc` subdirectory |
| `toolbox/CMakeLists.txt` | Add pef_loader, cfm_stubs, ppc_dispatch sources |
| `bin/CMakeLists.txt` | Link PPC_LIB, Unicorn |
| `bin/loader.cpp` | PPC detection, PEF loading, execution path |
| `mpw/mpw.cpp` | ECON device table, cookie setup, exit chain |
| `mpw/mpw.h` | New PPC-related declarations |
| `CMakeLists.txt` | Find Unicorn package at top level |

## Prerequisites

- Install Unicorn Engine: `brew install unicorn` (macOS) or build from source
- StdCLib PEF available at `~/mpw/SharedLibraries/` (or similar)
- Hello.c compiled with MPW's PPC compiler (PEF in data fork)

## End-to-End Verification

```bash
# Build
cd build && cmake .. && make

# Run Hello World
./bin/mpw --ppc ~/mpw/Tools/Hello

# Expected: "Hello, world!" on stdout, exit status 0
```
