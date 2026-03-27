# PPC Emulator Status

The MPW emulator can run PowerPC MPW tools alongside the existing 68K support. PPC execution uses the Unicorn Engine for CPU emulation, with real Mac OS shared libraries (StdCLib) running natively in the emulated environment.

## Quick Start

```bash
# Build
mkdir build && cd build && cmake .. && make

# Run a PPC tool
./bin/mpw --ppc tools/Hello

# Run with tracing
./bin/mpw --ppc --trace-toolbox tools/Hello    # stub dispatch
./bin/mpw --ppc --trace-mpw tools/Hello        # device I/O
./bin/mpw --ppc --trace-cpu tools/Hello        # every instruction

# Interactive debugger
./bin/mpw --ppc --debug tools/Hello
```

68K tools work as before (the `--ppc` flag is required for PPC).

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    PPC MPW Tool                         │
│                  (PEF executable)                       │
├─────────────────────────────────────────────────────────┤
│            StdCLib (real PEF shared library)            │
│  stdio, malloc, string ops, exit, setjmp/longjmp, ...  │
├──────────────┬──────────────┬───────────────────────────┤
│ InterfaceLib │   MathLib    │  PrivateInterfaceLib      │
│  (95 stubs)  │  (4 stubs)   │    (2 stubs)              │
├──────────────┴──────────────┴───────────────────────────┤
│              CFM Stub Dispatch (sc instruction)         │
├──────────────┬──────────────────────────────────────────┤
│  ECON device │  FSYS device    │  MPW::Native::*        │
│  (console)   │  (file I/O)     │  (shared with 68K)     │
├──────────────┴─────────────────┴────────────────────────┤
│           Unicorn Engine (PPC 603e, 32-bit BE)          │
├─────────────────────────────────────────────────────────┤
│              Shared emulated memory (16 MB)             │
│     ┌─────────┬────────┬───────────┬─────────┐         │
│     │ globals │  heap  │ mplite    │  stack   │         │
│     │ 0-64K   │ (pool) │ metadata  │ top 32K  │         │
│     └─────────┴────────┴───────────┴─────────┘         │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

- **Real StdCLib**: The actual Mac OS StdCLib PEF shared library runs natively in the PPC emulator. We do NOT intercept or reimplement C library functions. StdCLib handles stdio buffering, `setjmp`/`longjmp`, exit chains, and all C runtime behavior.

- **Stub libraries**: InterfaceLib, MathLib, and PrivateInterfaceLib are "stub libraries" — they have no code of their own. Our CFM stub system IS the implementation. Each imported function gets a `li r11,<index>; sc; blr` code sequence that triggers host-side dispatch.

- **Shared memory**: The PPC emulator shares the same 16 MB memory buffer as the 68K emulator (mapped into Unicorn via `uc_mem_map_ptr`). Memory Manager, file I/O, and all emulated state are shared.

- **Device handlers**: StdCLib accesses files and console through device handlers in the MPGM device table. ECON handles console I/O (stdin/stdout/stderr) with CR→LF conversion. FSYS handles file I/O via `MPW::Native::*` functions shared with the 68K path.

## Source Files

### CPU Layer
| File | Description |
|------|-------------|
| `cpu/ppc/ppc.h` | PPC API: Init, Execute, Step, register access, Disassemble |
| `cpu/ppc/ppc.cpp` | Unicorn Engine wrapper, sc interrupt hook, Capstone disassembly |

### PEF Loader
| File | Description |
|------|-------------|
| `toolbox/pef.h` | PEF format constants (magic tags, section kinds, symbol classes) |
| `toolbox/pef_loader.h` | LoadResult struct, LoadPEF function |
| `toolbox/pef_loader.cpp` | PEF container parser, pidata decompression, relocation engine |

### CFM / Import Dispatch
| File | Description |
|------|-------------|
| `toolbox/cfm_stubs.h` | RegisterStub, ResolveImport, Dispatch, AllocateCode |
| `toolbox/cfm_stubs.cpp` | sc stub generation, TVector management, catch-all handler |

### InterfaceLib Wrappers + Device Handlers
| File | Description |
|------|-------------|
| `toolbox/ppc_dispatch.h` | RegisterStdCLibImports, PatchDeviceTable |
| `toolbox/ppc_dispatch.cpp` | 95 InterfaceLib stubs, ECON/FSYS device handlers, cookie management |

### MPW Native File I/O (shared 68K/PPC)
| File | Description |
|------|-------------|
| `mpw/mpw.h` | MPW::Native::Access/Read/Write/Close/IOCtl |
| `mpw/mpw_access.cpp` | File open/delete/rename (faccess) |
| `mpw/mpw_io.cpp` | File read/write |
| `mpw/mpw_close.cpp` | File close |
| `mpw/mpw_ioctl.cpp` | ioctl: seek, dup, interactive, bufsize, refnum, seteof |

### Tools
| File | Description |
|------|-------------|
| `toolbox/pef_inspect.cpp` | Interactive PEF inspector CLI (sections, imports, exports, disasm, memory) |
| `tools/Hello.c` | Minimal PPC test tool (fprintf + return) |
| `tools/FileRead.c` | File I/O test tool (fopen/fread/fgetc) |

### Debugger (PPC extensions)
| File | Description |
|------|-------------|
| `bin/debugger.cpp` | PPC mode: step, registers (r0-r31, LR, CTR, CR, XER), PPC disassembly |
| `bin/lexer.rl` | PPC register tokens (r0-r31, lr, ctr, cr, xer, rtoc) |
| `bin/parser.lemon` | GPREGISTER grammar rules |

## What Works

### Fully Working
- PPC CPU execution (Unicorn 603e, MSR[FP]=1 for floating-point)
- PEF loading with pidata decompression and full relocation engine
- On-demand shared library loading (StdCLib, and any others the tool imports)
- StdCLib init/exit lifecycle (setjmp/longjmp exit mechanism)
- Console I/O (stdin/stdout/stderr) with CR→LF conversion
- File I/O (open/read/write/close/seek) via stdio and low-level APIs
- Handle-based cookies for IO table entries
- 95 InterfaceLib + 4 MathLib + 2 PrivateInterfaceLib stubs
- Interactive PPC debugger (step, break, registers, memory, disassembly)
- Exit code capture via MPW::ExitStatus()
- CallUniversalProc PPC trampoline (dispatches PPC TVectors and 68K descriptors)

### Tested Tools
| Tool | Status |
|------|--------|
| `tools/Hello` | Working. Prints "Hello, world!", exits 0. |
| `tools/FileRead` | Working. fopen/fread/fgetc all work correctly. |
| `~/mpw/Tools/DumpPEF` | Partial. Opens files, reads PEF headers, but crashes during section parsing (unmapped memory read at 0xF24A04). |

### Known Limitations
- **16 MB address space**: Tools + libraries + data must fit in 16 MB. Increase with `--memory`.
- **No resource fork I/O via stdio**: `fopen` with `O_RSRC` opens the native resource fork, but DumpPEF's PEF parsing of resource fork data has issues.
- **DumpPEF crash**: The PPC DumpPEF crashes with an unmapped memory read during PEF section string table parsing. The I/O layer works correctly — this is a tool-specific parsing issue, possibly related to buffer size or data interpretation.
- **Catch-all stub**: Any unimplemented InterfaceLib function prints a fatal error with register state and stops execution. Add new stubs to `ppc_dispatch.cpp` as needed.

## Building PPC Test Tools

PPC tools are compiled with MrC and linked with PPCLink — both run via the 68K emulator:

```bash
cd tools
make Hello          # or: make FileRead, make all
```

The `Makefile` uses:
```makefile
MrC = ../build/bin/mpw MrC
PPCLink = ../build/bin/mpw PPCLink
SHARED_LIBS = "{SharedLibraries}"StdCLib "{SharedLibraries}"InterfaceLib
PPC_LIBS = "{PPCLibraries}"StdCRuntime.o "{PPCLibraries}"PPCCRuntime.o "{PPCLibraries}"PPCToolLibs.o
```

Write a `.c` file, add a Makefile target, and `make` it. The resulting PEF binary runs with `./bin/mpw --ppc tools/YourTool`.

## Debugging

### Trace Flags
| Flag | Shows |
|------|-------|
| `--trace-toolbox` | Every sc stub call with args and return values |
| `--trace-mpw` | ECON/FSYS device handler calls (read/write/ioctl) |
| `--trace-cpu` | Every PPC instruction (very verbose) |

### Interactive Debugger
```bash
./bin/mpw --ppc --debug tools/Hello
] l                    # disassemble at PC
] s                    # step one instruction
] s 10                 # step 10 instructions
] c                    # continue until breakpoint
] b 0x00F1CC7C         # set breakpoint
] p r3                 # print register
] p *0x00F71790        # print memory word
] p pc                 # print PC
```

### pef_inspect Tool
```bash
./build/toolbox/pef_inspect ~/mpw/Libraries/SharedLibraries/StdCLib
> sections             # show section layout
> imports              # show imported symbols
> exports              # show exported symbols
> sym fprintf           # look up symbol
> disasm 0xF5541C 20   # disassemble at address
> tocoff 0x698          # look up TOC-relative offset
```

### Host-Side Debugging
Use `lldb` for crashes in the emulator itself (mplite, Unicorn, etc.):
```bash
lldb -- ./build-debug/bin/mpw --ppc tools/Hello
(lldb) b __assert_rtn        # catch assertion failures
(lldb) b ppc.cpp:164          # catch Unicorn errors
(lldb) run
```
