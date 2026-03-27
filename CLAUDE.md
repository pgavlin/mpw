# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MPW (Macintosh Programmer's Workshop) Emulator — emulates the MPW command-line environment for both Motorola 68000 and PowerPC CPUs, allowing classic Mac MPW tools to run on modern macOS and Linux. Both CPU architectures are emulated via the Unicorn Engine. PPC tools use the real Mac OS StdCLib shared library running natively. Requires libedit for the debugger. See `PPC_STATUS.md` for detailed PPC emulator documentation.

## Build Commands

```bash
# First-time setup (fetch libsane submodule)
git submodule init && git submodule update

# Build
mkdir build && cd build
cmake ..
make

# Outputs: build/bin/mpw (emulator), build/bin/disasm (disassembler), build/bin/mpw-lsp (LSP server)
```

**Required tools:** cmake (3.1+), ragel (version 6), lemon, clang++ with C++11 support.
**Required libraries:** libunicorn (PPC emulation), libcapstone (PPC disassembly), libedit (debugger readline).

## Tests

Tests in `test/` are MPW-hosted programs (compiled and linked using MPW tools via the emulator itself). They require a working MPW environment with libraries/tools installed. Run from `test/` with `make`.

## Architecture

The emulator has five main layers:

- **`cpu/m68k/`** — Motorola 680x0 CPU emulation via Unicorn Engine. `m68k_uc.cpp` wraps Unicorn with register access, A-line/F-line exception dispatch, and Capstone-based disassembly.

- **`cpu/ppc/`** — PowerPC CPU emulation via Unicorn Engine. `ppc.cpp` wraps Unicorn with PPC register access, `sc` interrupt handling for CFM stub dispatch, code trace/profiler hooks, and Capstone-based disassembly.

- **`toolbox/`** — Macintosh Toolbox/OS trap emulation. `dispatch.cpp` routes A-line traps to implementations across ~40 files covering Memory Manager (`mm.cpp`), Resource Manager (`rm.cpp`), OS calls (`os.cpp`, `os_hfs_dispatch.cpp`), SANE floating-point (`sane.cpp`), and more. PPC-specific: `ppc_dispatch.cpp` (95 InterfaceLib stubs + ECON/FSYS device handlers), `cfm_stubs.cpp` (sc-based import dispatch), `pef_loader.cpp` (PEF loader with pidata decompression and relocation engine).

- **`mpw/`** — MPW environment emulation: file I/O (`mpw_io.cpp`), environment variables, errno mapping. File I/O functions are factored into `MPW::Native::` versions shared by both 68K and PPC paths. The environment variable parser is generated from `environment.rl` (Ragel).

- **`macos/`** — System-level definitions: trap tables (`traps.c`), system equates (`sysequ.c`), error codes (`errors.cpp`).

- **`bin/`** — Executable entry points. `loader.cpp` loads MPW executables. `debugger.cpp` provides an interactive debugger with breakpoints and memory inspection. `profiler.cpp` generates callgrind-format execution traces (`--profile`). Command parsing uses Ragel (lexer) + Lemon (parser). Also contains Python scripts for HFS disk image tooling (`setup_mpw.py`, `package_hfs.py`).

- **`lsp/`** — Language Server Protocol server for MPW development tools. Translates MPW compiler error output into standard LSP diagnostics for IDE integration.

**`rsrc/`** — Standalone Mac resource fork parser and accessor. Reads resource forks via `path/..namedfork/rsrc` on macOS or AppleDouble sidecar files on other platforms. No Apple framework dependencies.

**`macos_compat.h`** — Cross-platform compatibility header. Abstracts macOS-specific APIs (endianness macros, xattr signatures, `setattrlist`) for Linux portability.

**`toolbox/path_utils.cpp`** — Case-insensitive path resolution. Bridges the gap between case-insensitive Mac paths and case-sensitive Unix filesystems.

**`mplite/`** — Vendored zero-alloc memory pool allocator (from SQLite/mempoolite), used by the Memory Manager.

## Code Generation

Several source files are generated at build time:
- **Ragel** (`.rl` → `.cpp`): lexer, environment parser, pathname parser, loadtrap, template_loader
- **Lemon** (`.lemon` → `.cpp` + `.h`): parser, template_parser

Edit the `.rl`/`.lemon` source files, not their generated outputs.

## Analyzing PowerPC Binaries

Use the MPW `DumpPEF` tool to inspect PPC executables and shared libraries:

```bash
mpw DumpPEF -do All -pi u -a -fmt on <path-to-pef>
```

This produces full information for all sections including code section disassembly, unpacked data sections, import/export tables, and relocation info.

## License Constraints

The CPU emulation code (`cpu/`) is GPL v2+. All other code is BSD 2-Clause, but the compiled binary is GPL v2 due to linking.
