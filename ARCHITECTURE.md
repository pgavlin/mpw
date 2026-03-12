# MPW Emulator — Architecture

MPW (Macintosh Programmer's Workshop) was Apple's command-line development environment for classic Mac OS. This emulator runs MPW tools (compilers, linkers, assemblers) on modern macOS and Linux by emulating both the Motorola 68030 CPU and the Mac OS Toolbox APIs they depend on.

## Directory Structure

```
mpw/
├── bin/          Entry points: emulator (loader.cpp), disassembler, debugger,
│                 plus Python scripts for HFS image tooling
├── cpu/          Motorola 680x0 CPU emulation (from WinFellow)
├── toolbox/      Macintosh Toolbox/OS trap implementations (~40 files)
├── mpw/          MPW environment emulation (file I/O, env vars, errno mapping)
├── macos/        System definitions (trap tables, system equates, error codes)
├── rsrc/         Standalone resource fork parser and platform accessor
├── mplite/       Vendored memory pool allocator (from SQLite/mempoolite)
├── libsane/      SANE floating-point library (git submodule)
├── cxx/          C++ utility code
├── macos_compat.h  Cross-platform compatibility layer (endianness, xattr, etc.)
└── test/         MPW-hosted test programs
```

## Memory Layout

The emulator allocates a flat byte array (default 16 MB) representing the 68K address space:

```
0x00000000 ┌──────────────────────┐
           │   System Globals     │  Mac low-memory globals
           │   (CurrentA5, etc.)  │  (CurJTOffset, ApplLimit, ...)
0x00010000 ├──────────────────────┤
           │                      │
           │   Heap               │  Handles and pointers from
           │                      │  the Memory Manager
           │                      │
           ├──────────────────────┤  ← ApplLimit
           │                      │
           │   Stack              │  68K program stack
           │   (grows downward)   │  (A7 starts at top)
           │                      │
           └──────────────────────┘  ← MemorySize
```

All memory access from the CPU goes through bounds-checked functions (`memoryReadByte/Word/Long`, `memoryWriteByte/Word/Long`). Out-of-bounds reads return 0; out-of-bounds writes are silently dropped.

## Startup Flow

The main entry point is `bin/loader.cpp`. Startup proceeds as follows:

1. **Parse arguments** — RAM size, stack size, trace flags, debugger mode.
2. **Find the tool** — `find_exe()` searches the `$Commands` path variable for the named executable.
3. **Initialize subsystems:**
   - `MM::Init()` — Memory Manager (heap, handle/pointer tracking)
   - `OS::Init()` — OS module
   - `ToolBox::Init()` — Toolbox
   - `MPW::Init(argc, argv)` — MPW environment, file descriptors, argv
4. **Initialize the CPU** — `cpuStartup()`, set model to 68030.
5. **Load the program** — `Loader::Native::LoadFile()` opens the executable's resource fork, reads CODE resources, builds the jump table, applies relocations, and sets PC to the entry point.
6. **Initialize globals** — `GlobalInit()` writes system globals (Lo3Bytes, MinusOne, ApplLimit, CurrentA5, etc.) into low memory.
7. **Register trap handlers:**
   - A-line traps (`0xAxxx`) → `ToolBox::dispatch`
   - F-line traps (`0xFxxx`) → `MPW::dispatch`
8. **Run** — either `MainLoop()` (execute until halt) or `Debug::Shell()` (interactive debugger).

`MainLoop()` repeatedly calls `cpuExecuteInstruction()` until `cpuGetStop()` returns true. It validates the stack pointer and PC on each iteration.

## CPU Emulation (`cpu/`)

Adapted from the **WinFellow** Amiga emulator (GPL v2). The core consists of large generated C files implementing full 68000/010/020/030 instruction decode and execution.

### Key Files

| File | Purpose |
|------|---------|
| `CpuModule.h` | Public CPU interface |
| `CpuModule.c` | Initialization (`cpuStartup`, `cpuHardReset`) |
| `CpuModule_Instructions.c` | Instruction execution (generated, ~4000 lines) |
| `CpuModule_Disassembler.c` | Instruction disassembly |
| `CpuModule_EffectiveAddress.c` | Address mode calculation |
| `CpuModule_Exceptions.c` | Exception/trap handling |
| `CpuModule_Interrupts.c` | Interrupt dispatch |
| `CpuModule_Flags.c` | Condition code computation |
| `CpuModule_InternalState.c` | Register file, prefetch, state save/restore |
| `CpuIntegration.c` | Integration layer between CPU and host |

### Execution Model

```
cpuExecuteInstruction()
  ├── Check pending interrupts → cpuSetUpInterrupt() if raised
  ├── Fetch opcode at PC (16-bit word)
  ├── Lookup in cpu_opcode_data_current[] function pointer table
  ├── Execute instruction handler (updates regs, flags, PC)
  └── Return cycle count
```

Special opcodes trigger exceptions rather than normal execution:
- **A-line** (`0xAxxx`) — calls the registered A-line exception handler (Toolbox traps)
- **F-line** (`0xFxxx`) — calls the registered F-line exception handler (MPW I/O traps)

### Supported Models

The CPU model is set via `cpuSetModel(major, minor)`:

| major | minor | Model |
|-------|-------|-------|
| 0 | 0 | 68000 |
| 1 | 0 | 68010 |
| 2 | 0 | 68020 |
| 3 | 0 | 68030 (default) |

## Trap Dispatch (`toolbox/dispatch.cpp`)

When the 68K CPU encounters an A-line instruction, control transfers to `ToolBox::dispatch()`. The trap word encodes the call:

```
  1010  T  xx  S  nnnnnnnn
  ────  ─  ──  ─  ────────
  A-line│  │   │  Trap number
        │  │   Save A0 (OS traps)
        │  Reserved
        Toolbox (1) vs OS (0)
```

### Dispatch Flow

1. Extract the trap number from the 16-bit opcode.
2. Check modifier bits — auto-pop (bit 10), save A0 (bits 8–9).
3. If a patched trap address exists in `trap_address[]` or `os_address[]`, transfer control to the 68K routine at that address.
4. Otherwise, call the native C++ handler via `native_dispatch()`.
5. Set D0 with the return value and update condition codes.

### Implemented Managers

| Manager | File(s) | Example Traps |
|---------|---------|---------------|
| Memory Manager | `mm.cpp` | NewHandle, DisposeHandle, NewPtr, HLock, HUnlock |
| Resource Manager | `rm.cpp` | OpenResFile, CloseResFile, GetResource, Get1Resource |
| File System | `os.cpp`, `os_hfs_dispatch.cpp` | Open, Close, Read, Write, FSMakeFSSpec |
| SANE (FP math) | `sane.cpp` | fp68k, decstr68k |
| Strings/Utility | `utility.cpp` | Munger, NewString, CmpString, RelString |
| QuickDraw (stubs) | `qd.cpp` | InitGraf (mostly no-ops for CLI tools) |
| Segment Loader | `loader.cpp` | LoadSeg, UnloadSeg |
| Packages | `package.cpp` | Pack7 (NumToString, StringToNum) |
| Sound (stubs) | `sound.cpp` | SysBeep |
| Debug | `debug.cpp` | DebugStr |

Programs can dynamically patch traps via `SetTrapAddress`/`GetTrapAddress`. The dispatcher maintains two arrays for this:
```cpp
static uint32_t trap_address[1024];   // Toolbox trap overrides
static uint32_t os_address[256];      // OS trap overrides
```

## Program Loading (`toolbox/loader.cpp`)

MPW executables use the classic Mac **CODE resource** format stored in the file's resource fork.

### CODE Resource Structure

- **CODE 0** — Jump table and A5 world descriptor:
  ```
  Offset 0:   Above-A5 size    (uint32)
  Offset 4:   Below-A5 size    (uint32)
  Offset 8:   Jump table size  (uint32)
  Offset 12:  Jump table offset from A5  (uint32)
  Offset 16+: Jump table data
  ```
- **CODE 1+** — Code segments containing 68K machine code.

### Loading Process

1. **Open resource fork** — `RM::Native::OpenResourceFile()` reads the resource fork and parses it.
2. **Load CODE 0** — extract the A5 world descriptor, allocate memory for the above-A5 and below-A5 regions, copy the jump table.
3. **Build jump table** — iterate through 8-byte jump table entries. Each references a CODE segment and offset. Load the segment if needed, resolve the address, and patch the entry:
   ```
   Before: [offset:16] [segment:16] [0x3F3C] [segment:16] [0xA9F0]
   After:  [segment:16] [JMP.L addr:32] [padding]
   ```
4. **Relocate far-model segments** — walk relocation tables and fix up absolute addresses.
5. **Set registers** — A5 = application globals base, PC = first jump table entry (main entry point).

## MPW Environment (`mpw/`)

### Environment Variables

The MPW environment is initialized from multiple sources:
1. Locate the MPW root directory (`$MPW` env var, `~/mpw`, `/usr/local/share/mpw`, or `/usr/share/mpw`).
2. Parse `Environment.text` from the MPW root. The syntax supports assignments, conditionals, and variable expansion:
   ```
   Commands    = {MPW}Tools:,{MPW}Scripts:
   COptions    ?= -mc68020
   Libraries   = {MPW}Libraries:
   ```
   The parser is generated from `environment.rl` (Ragel).
3. Overlay host environment variables and command-line `-D` defines.

Key variables: `$MPW`, `$Commands`, `$COptions`, `$Libraries`, `$PLibraries`, `$Interfaces`.

### File I/O

F-line traps (`0xFxxx`) handle MPW-specific system calls:

| Trap | Operation |
|------|-----------|
| `fQuit` | Exit program with status |
| `fAccess` | Open / check file access |
| `fClose` | Close file descriptor |
| `fRead` | Read from file |
| `fWrite` | Write to file |
| `fIOCtl` | File control (e.g., set binary mode) |

The I/O layer converts Mac-style colon-delimited paths to Unix paths and maps Mac OS error codes to errno values. stdin, stdout, and stderr are connected to the host terminal.

## Cross-Platform Compatibility (`macos_compat.h`)

A single header that abstracts platform-specific macOS APIs for Linux portability:

- **Endianness macros** — maps Linux `<endian.h>` to macOS-style names (`BYTE_ORDER`, `BIG_ENDIAN`, `LITTLE_ENDIAN`).
- **Extended attribute wrappers** — adapts the macOS 6-argument `getxattr`/`setxattr`/`fgetxattr` signatures to Linux's 4-argument versions, and defines xattr name constants (`com.apple.FinderInfo`, `com.apple.ResourceFork`).
- **Resource fork path** — defines `_PATH_RSRCFORKSPEC` (`/..namedfork/rsrc`) on Linux where it doesn't exist natively.
- **`setattrlist()` stub** — returns `ENOTSUP`, allowing callers to fall back to `utimes()`.

This eliminates scattered `#ifdef __APPLE__` blocks throughout the codebase.

## Case-Insensitive Path Resolution (`toolbox/path_utils.cpp`)

Classic Mac paths are case-insensitive, but Unix filesystems are case-sensitive. `resolve_path_ci()` bridges this gap:

1. **Fast path** — tries the path as-is via `stat()`; returns immediately if found.
2. **Slow path** — on `ENOENT`, walks each directory component and performs a case-insensitive `readdir()` scan to find the actual on-disk casing.
3. **Create mode** — when `resolve_leaf=false`, only resolves the directory portion, leaving the final component unresolved for file creation.

## Resource Fork Access (`rsrc/`)

A standalone library with no Apple framework dependencies that reads and writes Mac resource forks.

### Platform Support

- **macOS** — reads/writes via the named fork path: `path/..namedfork/rsrc`, falling back to AppleDouble sidecars.
- **Linux and other platforms** — reads/writes AppleDouble sidecar files (`._filename`). For writable access, extracts the resource fork to a temporary file and writes modifications back on close.

### Resource Fork Binary Format

```
Offset 0x00:  Data offset     (uint32, big-endian)
Offset 0x04:  Map offset      (uint32)
Offset 0x08:  Data length     (uint32)
Offset 0x0C:  Map length      (uint32)

Data section:
  Each resource: [length:32] [data:length]

Map section:
  Offset 0x00:  (copy of header, 16 bytes)
  Offset 0x10:  Next map handle (uint32)
  Offset 0x14:  File ref       (uint16)
  Offset 0x16:  File attributes (uint16)
  Offset 0x18:  Type list offset from map start (uint16)
  Offset 0x1A:  Name list offset from map start (uint16)
  Offset 0x1C:  Type count - 1 (uint16)

  Type list entries:
    [type:32] [count-1:16] [ref list offset from type list:16]

  Reference list entries:
    [id:16] [name offset:16] [attributes:8] [data offset:24]
```

All values are big-endian. Resource data loaded into emulator memory does not need byte-swapping since the emulated 68K CPU is also big-endian.

### AppleDouble Sidecar Format

On non-macOS platforms (and as a fallback on macOS), resource forks and Finder Info are stored in AppleDouble sidecar files named `._<filename>`. The accessor layer parses these files to extract entry ID 2 (resource fork) and entry ID 9 (Finder Info), and can update individual entries without clobbering others.

### API

```cpp
// Read resource fork bytes from the appropriate platform source
std::vector<uint8_t> readResourceFork(const std::string &path);

// Parse fork bytes into a queryable structure
std::unique_ptr<ResourceFile> ResourceFile::open(const std::vector<uint8_t> &data);

// Query resources
const ResourceEntry *findResource(uint32_t type, int16_t id);
std::vector<uint8_t> loadResource(const ResourceEntry &entry);
int countResources(uint32_t type);
```

## Memory Manager (`toolbox/mm.cpp`)

The Memory Manager provides two allocation primitives:

- **Pointers** (`NewPtr`/`DisposePtr`) — fixed-address allocations, tracked in a `std::map<uint32_t, uint32_t>`.
- **Handles** (`NewHandle`/`DisposeHandle`) — double-indirect references that can be relocated, locked, or purged.

The backing allocator is **mplite** (`mplite/`), a zero-allocation memory pool adapted from SQLite. It operates within the emulated heap region.

Handle metadata:
```cpp
struct HandleInfo {
    uint32_t address;    // Pointer to data
    uint32_t size;       // Data size
    bool locked;         // Prevent relocation
    bool purgeable;      // Allow system to free
    bool resource;       // Belongs to Resource Manager
};
```

## Debugger (`bin/debugger.cpp`)

An interactive debugger activated with the `--debugger` flag. Uses libedit/readline for line editing and history.

### Capabilities

- **Breakpoints** — execution (by address), memory read, memory write, and trap breakpoints.
- **Stepping** — single instruction step, continue to breakpoint or halt.
- **Inspection** — register dump (D0–D7, A0–A7, PC, SR), memory hex dump, disassembly at arbitrary addresses.
- **Symbols** — MacsBug name lookup from CODE segment metadata for symbolic disassembly.
- **Templates** — data structure definitions for interpreting memory regions as typed fields.
- **Backtrace** — circular buffer of recent CPU state for post-mortem analysis.

Command parsing uses a Ragel-generated lexer (`debugger_lexer.rl`) and a Lemon-generated parser (`debugger_parser.lemon`).

## HFS Disk Image Tooling (`bin/`)

Two Python scripts support bootstrapping and packaging the MPW environment without platform-specific tools:

### `setup_mpw.py` — Extract MPW from Disk Images

Extracts MPW tools, interfaces, and libraries from classic Mac HFS disk images. Includes a from-scratch HFS parser that reads B-tree catalog and extents overflow structures directly from raw image bytes. Supports raw HFS, Apple Partition Map, MacBinary wrapping, and NDIF (DiskCopy 6.x with ADC decompression).

Preserves resource forks and Finder Info: on macOS via xattr, on Linux via AppleDouble sidecar files.

### `package_hfs.py` — Create HFS Disk Images

The inverse of `setup_mpw.py`: builds valid HFS disk images from directory trees. Reads AppleDouble sidecars (or native resource forks on macOS) to preserve Mac metadata. Constructs complete HFS volumes including B-tree nodes, allocation block management, and Master Directory Blocks.

## Code Generation

Several source files are generated at build time. Always edit the source files, not the generated output:

| Source | Generator | Output |
|--------|-----------|--------|
| `debugger_lexer.rl` | Ragel | `debugger_lexer.cpp` |
| `debugger_parser.lemon` | Lemon | `debugger_parser.cpp` + `.h` |
| `environment.rl` | Ragel | `environment.cpp` |
| `pathnames.rl` | Ragel | `pathnames.cpp` |
| `loadtrap.rl` | Ragel | `loadtrap.cpp` |
| `template_loader.rl` | Ragel | `template_loader.cpp` |
| `template_parser.lemon` | Lemon | `template_parser.cpp` + `.h` |

## License

The CPU emulation code (`cpu/`) is GPL v2+. All other code is BSD 2-Clause. The compiled binary is GPL v2 due to linking.
