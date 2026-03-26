# Phase 5: PPC Tool Loading and Execution

**Goal:** Wire everything together in `bin/loader.cpp` to detect, load, and run a PPC MPW tool end-to-end.

**Depends on:** Phases 1-4 (PPC CPU, PEF loader, CFM stubs, InterfaceLib wrappers).

---

## Overview

This phase connects the PPC CPU (Phase 1), PEF loader (Phase 2), CFM stubs (Phase 3), and InterfaceLib wrappers (Phase 4) into a complete execution path. StdCLib init will crash when it reaches ECON device calls (not yet implemented) — that's expected and will be fixed in Phase 6. The loader must:

1. Detect whether the tool is a PEF or 68K executable
2. Initialize the PPC subsystem
3. Load the tool PEF — shared libraries are loaded on demand when the tool's imports reference them
4. Run shared library init routines, then the tool
5. Capture the exit code

---

## Execution Flow (Detailed)

### Step 1: Detect PEF

Check the data fork of the tool for PEF magic (`'Joy!' 'peff'`). Also support `--ppc` / `--68k` flags for explicit override.

```cpp
static bool IsPEFFile(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t header[8];
    size_t n = fread(header, 1, 8, f);
    fclose(f);
    return n == 8 && PEFLoader::IsPEF(header, 8);
}
```

Add to the Flags structure in `loader.h`:
```cpp
bool forcePPC = false;
bool force68K = false;
```

Add `--ppc` and `--68k` to the getopt_long option table.

Decision logic:
```cpp
bool usePPC;
if (Flags.forcePPC) usePPC = true;
else if (Flags.force68K) usePPC = false;
else usePPC = IsPEFFile(command);
```

### Step 2: Initialize Subsystems

Same as the 68K path:
```cpp
MM::Init(Memory, MemorySize, kGlobalSize, Flags.stackSize);
OS::Init();
ToolBox::Init();
MPW::Init(argc, argv);
```

### Step 3: Initialize PPC

```cpp
PPC::Init(Memory, MemorySize);
CFMStubs::Init();
PPC::SetSCHandler(CFMStubs::Dispatch);
PPCDispatch::RegisterStdCLibImports();
```

### Step 4: Load the Tool PEF (with on-demand library loading)

The tool's import resolver drives all shared library loading. When the tool imports from "StdCLib", the resolver loads StdCLib's PEF, resolves StdCLib's own imports (from InterfaceLib/MathLib — our CFM stubs), registers StdCLib's exports, and returns the requested symbol's address.

Stub libraries (InterfaceLib, MathLib, PrivateInterfaceLib) are never loaded as PEFs — our CFM stubs from Phase 4 ARE their implementation.

```cpp
// Track loaded libraries: name → LoadResult
std::map<std::string, PEFLoader::LoadResult> loadedLibs;

// Resolver that loads real libraries on demand
auto resolver = [&](const std::string &lib, const std::string &sym,
                     uint8_t cls) -> uint32_t {
    // First check if already resolved (CFM stubs or previously loaded library)
    uint32_t addr = CFMStubs::ResolveImport(lib, sym);
    if (addr) return addr;

    // Try to load the library PEF if not yet loaded
    if (loadedLibs.find(lib) == loadedLibs.end()) {
        std::string libPath = findSharedLibrary(lib);
        if (!libPath.empty()) {
            PEFLoader::LoadResult libResult;
            // Libraries' own imports resolve against CFM stubs (recursive)
            bool ok = PEFLoader::LoadPEFFile(libPath, resolver, libResult);
            if (ok) {
                // Register all exports from this library
                for (const auto &exp : libResult.exports) {
                    if (exp.sectionIndex < libResult.sections.size()) {
                        uint32_t a = libResult.sections[exp.sectionIndex].address
                                     + exp.offset;
                        CFMStubs::RegisterTVector(lib, exp.name, a);
                    }
                }
                loadedLibs[lib] = std::move(libResult);

                // Retry the lookup now that exports are registered
                addr = CFMStubs::ResolveImport(lib, sym);
                if (addr) return addr;
            }
        }
    }

    // Register catch-all for truly unresolved imports
    return CFMStubs::RegisterStub(lib, sym, [lib, sym]() {
        fprintf(stderr, "PPC FATAL: unimplemented stub %s::%s called\n",
                lib.c_str(), sym.c_str());
        fprintf(stderr, "  r3=0x%08X r4=0x%08X LR=0x%08X\n",
                PPC::GetGPR(3), PPC::GetGPR(4), PPC::GetLR());
        PPC::Stop();
    });
};

PEFLoader::LoadResult toolResult;
bool ok = PEFLoader::LoadPEFFile(command, resolver, toolResult);
```

This naturally handles any library, not just StdCLib. If a tool imports from a library we have stubs for (InterfaceLib), those resolve immediately. If it imports from a real library (StdCLib), the PEF is loaded on demand.

### Step 5: Set Up Exit Chain and Patch Device Table

After loading, look up `__target_for_exit` from whichever library exports it (StdCLib):

```cpp
// Find __target_for_exit from loaded libraries
uint32_t targetForExit = 0;
for (auto &[name, lib] : loadedLibs) {
    uint32_t addr = PEFLoader::FindExport(lib, "__target_for_exit");
    if (addr) { targetForExit = addr; break; }
}
if (targetForExit) setupExitChain(targetForExit);  // Phase 6

// Patch device table for PPC (Phase 6)
// PPCDispatch::PatchDeviceTable(...);
```

### Step 6: Run Library Init Routines

Run `__initialize` for each loaded library that has an init entry:

```cpp
for (auto &[name, lib] : loadedLibs) {
    if (lib.initPoint) {
        PPCCallFunction(lib.initPoint);
    }
}
```

### Step 7: Run the Tool

```cpp
if (toolResult.entryPoint) {
    PPCCallFunction(toolResult.entryPoint);
}
```

The tool's entry point is typically `__start` (in the tool's own code section). `__start`:
1. Calls `setjmp(__target_for_exit)`
2. Loads argc/argv from MPGM
3. Calls `main(argc, argv)`
4. Calls `exit(return_value)`
5. `exit()` → `_RTExit` → `longjmp(__target_for_exit, 1)` → returns to `__start`
6. `__start` reads `*_exit_status` and returns (`blr`)

### Step 8: Capture Exit Code

```cpp
uint32_t rv = MPW::ExitStatus();  // reads info+0x0E
if (rv > 0xff) rv = 0xff;
exit(rv);
```

---

## PPCCallFunction Helper

This helper calls a PPC function by its TVector address:

```cpp
static void PPCCallFunction(uint32_t tvecAddr) {
    uint32_t codeAddr = memoryReadLong(tvecAddr);
    uint32_t toc = memoryReadLong(tvecAddr + 4);

    // Set up PPC stack pointer (r1) at top of memory, below 68K stack
    uint32_t ppcStackTop = Flags.memorySize - Flags.stackSize - 64;
    ppcStackTop &= ~0xF;  // 16-byte aligned
    PPC::SetGPR(1, ppcStackTop);

    // Set TOC
    PPC::SetGPR(2, toc);

    // Set LR = 0 as "return to top" sentinel
    PPC::SetLR(0);

    // Execute
    PPC::Execute(codeAddr, toc);
}
```

When the function returns (`blr` with LR=0), PC becomes 0 and execution stops (Unicorn raises unmapped fetch error).

---

## Finding Shared Libraries

StdCLib and other shared libraries are stored in the MPW environment. We need a search function:

```cpp
static std::string findSharedLibrary(const std::string &name) {
    // Check MPW SharedLibraries directory
    std::string mpwRoot = MPW::RootDir();

    // Try common paths
    std::vector<std::string> searchPaths = {
        mpwRoot + "/Libraries/SharedLibraries/" + name,
        mpwRoot + "/SharedLibraries/" + name,
        mpwRoot + "/Libraries/" + name,
    };

    for (const auto &path : searchPaths) {
        // Check data fork existence
        struct stat st;
        if (stat(path.c_str(), &st) == 0) return path;

        // Check with common suffixes
        // PEF shared libraries may not have an extension
    }

    return "";
}
```

The exact path depends on how the user's `~/mpw` is organized. May need `--shared-lib-path` flag or `$SharedLibraries` environment variable.

---

## Debugging and Trace Integration

### Trace Flag Wiring

The existing trace flags are reused for PPC — no new flags needed. When the PPC execution path is active, the existing flags are wired to the PPC trace infrastructure:

| Existing flag | PPC behavior |
|------|---------------|
| `--trace-cpu` | Every PPC instruction (address + hex + register snapshot). Very verbose. |
| `--trace-toolbox` | CFM stub dispatch (function name + args + return) AND PEF loader logging. One line per OS/Toolbox call. |
| `--trace-mpw` | ECON device handler calls (read/write/ioctl with fd, cmd, data). |
| `--debug` | Interactive debugger with PPC register display. |

`--trace-toolbox` is the one you'll use most often — it shows the entire API call sequence StdCLib makes during initialization and execution.

### Debugger Support

The existing interactive debugger (`bin/debugger.cpp`) supports breakpoints, memory inspection, register display, and single-stepping for 68K. For PPC, add basic debugger integration:

**Phase 5 scope (minimal):**
- `--debug` flag should work with PPC tools: drop into debugger before first PPC instruction
- Register display: show r0-r31, f0-f13, PC, LR, CTR, CR, XER, r2 (TOC)
- Memory inspection: already works (shared memory)
- `PPC::SetBreakpoint(addr)` — use `UC_HOOK_CODE` with a specific address range to break

**Deferred (not needed for Hello World):**
- PPC single-stepping (can be done with `uc_emu_start(... count=1)`)
- PPC disassembly in debugger (would need a PPC disassembler, or use `DumpPEF`)
- Breakpoints within StdCLib code

### Memory Watchpoints

For debugging the exit chain and stdout buffering, add memory write watchpoints:

```cpp
// Watch writes to the stdout FILE flags field:
PPC::SetWatchpoint(stdoutFileAddr + 0x12, 2, [](uint32_t addr, uint32_t val) {
    fprintf(stderr, "  WATCH: stdout flags written: 0x%04X\n", val);
});

// Watch writes to info+0x0E (exit code):
PPC::SetWatchpoint(MacProgramInfo + 0x0E, 4, [](uint32_t addr, uint32_t val) {
    fprintf(stderr, "  WATCH: exit code written: %d\n", (int32_t)val);
});
```

Implement via `UC_HOOK_MEM_WRITE` with address range filtering.

---

## Files to Modify

### `bin/loader.h`

Add to the `Settings` struct:
```cpp
bool forcePPC = false;
bool force68K = false;
```

The existing `traceCPU`, `traceToolBox`, `traceMPW`, and `debugger` flags are reused as-is.

### `bin/loader.cpp`

Major changes:

1. **Add command-line options**: `--ppc`, `--68k`
2. **Add PEF detection**: `IsPEFFile()` function
3. **Add PPC execution path**: After the existing 68K loading path, add an `if (usePPC)` branch with:
   - PPC::Init
   - Wire trace flags: `PPC::SetTraceCode(Flags.traceCPU)`, `CFMStubs::SetTrace(Flags.traceToolBox)`, `PEFLoader::SetTrace(Flags.traceToolBox)`
   - CFMStubs::Init
   - PPC::SetSCHandler
   - PPCDispatch::RegisterStdCLibImports
   - Load StdCLib
   - Register exports
   - Set up exit chain
   - Patch device table
   - Run StdCLib init
   - Load tool
   - Run tool
   - Capture exit code
4. **Add PPCCallFunction helper**
5. **Add shared library search**
6. **Add includes**: ppc.h, pef_loader.h, cfm_stubs.h, ppc_dispatch.h

### `bin/CMakeLists.txt`

Add `PPC_LIB` to the mpw target link libraries (already done in Phase 1).

---

## Implementation Steps

1. Add `--ppc` / `--68k` flags to getopt option table in loader.cpp
2. Add `IsPEFFile()` function
3. Add `findSharedLibrary()` function
4. Add `PPCCallFunction()` helper
5. Add the PPC execution path in `main()`:
   a. Detect PEF
   b. Initialize PPC subsystem
   c. Load StdCLib
   d. Register StdCLib exports
   e. Set up exit chain
   f. Patch device table
   g. Run StdCLib __initialize
   h. Load tool PEF
   i. Run tool entry point
   j. Capture exit code
6. Add new includes and link flags

---

## Validation

### End-to-end test: Hello World

```bash
cd build && cmake .. && make
./bin/mpw --ppc ~/path/to/Hello
```

**Expected output:**
```
Hello, world!
```

**Expected exit code:** 0 (`echo $?`)

### Trace test

Run with tracing to see the full execution flow:

```bash
./bin/mpw --ppc --trace-toolbox ~/path/to/Hello
```

Should show:
1. StdCLib imports being resolved
2. StdCLib __initialize stub calls (Gestalt, NewPtr, NewRoutineDescriptor, ECON ioctls)
3. Tool imports being resolved
4. Tool execution: fprintf → StdCLib internal calls → ECON write
5. Exit path: exit → _RTExit → longjmp → __start returns

### Failure mode: Missing stubs

If StdCLib imports a function we haven't implemented:
```
PPC: unresolved import InterfaceLib::SomeFunction
```

Add the missing stub and retry.

### Failure mode: StdCLib init crashes

If `__initialize` crashes:
1. Enable `CFMStubs::SetTrace(true)` to see the last stub call before the crash
2. Check PPC register state (PC, LR, r1) at crash point
3. Common causes: missing stub, wrong argument passing, bad memory access

### Failure mode: No output

If the tool runs but produces no output:
1. stdout may not be line-buffered → `\r` doesn't trigger flush
2. Exit may crash before flushing → data is lost in FILE buffer
3. Check Phase 6's ECON ioctl validation
4. Try HelloFlush (with explicit `fflush`) as a simpler test

### Failure mode: Exit crashes

If `exit(0)` crashes:
1. The exit chain (info+0x24) may be wrong → Phase 7 debugging
2. `longjmp` target may be corrupted
3. Enable instruction tracing around `_RTExit` code offset 0xF050

---

## Risk Notes

- **Shared library path**: The exact location of StdCLib in `~/mpw` may vary. Add helpful error messages if not found.
- **Multiple shared libraries**: The tool may import from InterfaceLib directly (not just via StdCLib). These are handled by CFM stubs, not by loading InterfaceLib as a PEF.
- **PPC stack setup**: The PPC stack must not overlap with the 68K stack or any allocated memory. Place it carefully.
- **Re-entrance**: `PPCCallFunction` is called twice (StdCLib init, then tool). The PPC engine state (registers) must be properly reset between calls.
- **Fat binaries**: Some tools have both 68K (CODE resources) and PPC (PEF data fork) code. The `--ppc` flag forces PPC; auto-detection should prefer PPC if PEF is present (or 68K if CODE resources exist — this is a policy choice).
