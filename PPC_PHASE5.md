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

### Step 5: Run Library Init Routines

Run `__initialize` for each loaded library that has an init entry:

```cpp
for (auto &[name, lib] : loadedLibs) {
    if (lib.initPoint) {
        PPCCallFunction(lib.initPoint);
    }
}
```

### Step 6: Run the Tool

```cpp
if (toolResult.entryPoint) {
    PPCCallFunction(toolResult.entryPoint);
}
```

See `PPC_EXIT_MECHANISM.md` for the full `__start` → `setjmp` → `main` → `exit` → `longjmp` flow.

Exit code capture (`MPW::ExitStatus()` reading `info+0x0E`) is MPW-specific and handled in Phase 6.

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
   - Load tool PEF (on-demand library loading via resolver)
   - Run library init routines
   - Run tool
4. **Add PPCCallFunction helper**
5. **Add shared library search**: `findSharedLibrary()`
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
   b. Initialize PPC subsystem (PPC::Init, CFMStubs, wrappers, trace flags)
   c. Load tool PEF with on-demand library resolver
   d. Run library init routines
   e. Run tool entry point
6. Add new includes

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```

### PEF detection
```bash
./bin/mpw --ppc tools/Hello        # should enter PPC path
./bin/mpw --68k ~/mpw/Tools/Canon  # should enter 68K path as before
```

### On-demand library loading

Run with `--trace-toolbox` to verify:
1. Tool PEF loaded, imports enumerated
2. StdCLib PEF loaded on demand when first StdCLib import is encountered
3. StdCLib's own imports (InterfaceLib, MathLib) resolve against CFM stubs
4. All StdCLib exports registered

```bash
./bin/mpw --ppc --trace-toolbox tools/Hello
```

### StdCLib init (expected to crash)

StdCLib's `__initialize` will be called and will crash when it reaches ECON device calls (not implemented until Phase 6). With `--trace-toolbox`, the trace should show stub calls up to the crash point. This confirms the integration harness works — Phase 6 fills in the missing pieces.

### 68K regression

Verify existing 68K tools still work:
```bash
./bin/mpw DumpPEF 2>&1 | head -1  # should print usage
```

---

## Risk Notes

- **Shared library path**: The exact location of shared libraries in `~/mpw` may vary. Add helpful error messages if not found.
- **Recursive resolver**: The on-demand resolver calls `LoadPEFFile` recursively (tool → StdCLib → InterfaceLib stubs). Ensure no infinite recursion if a library can't be found (the `loadedLibs` map check prevents re-loading, but a library that imports from itself would loop).
- **PPC stack setup**: The PPC stack must not overlap with the 68K stack or any allocated memory. Place it carefully.
- **Re-entrance**: `PPCCallFunction` is called multiple times (library inits, then tool). The PPC engine state (registers) must be properly reset between calls.
- **Fat binaries**: Some tools have both 68K (CODE resources) and PPC (PEF data fork) code. The `--ppc` flag forces PPC; auto-detection should prefer PPC if PEF is present.
