---
name: classic-mac-dev
description: >
  Context for classic Macintosh application development using MPW tools.
  Use when writing, compiling, linking, or debugging classic Mac programs;
  when working with resource forks, Toolbox traps, or 68K code;
  or when you need to understand how classic Mac applications are structured.
---

# Classic Macintosh Development with MPW

You are helping develop classic Macintosh applications. The build environment
is a modern macOS or Linux host. Classic Mac tools (compilers, linker, Rez,
assembler) run through the `mpw` emulator, but the overall build process uses
standard Unix tools (make, shell scripts, etc.). You do NOT need MPW's own
shell, Make, or scripting—use their modern equivalents.

## Execution Model

```
modern shell
  └─ make / shell scripts      (orchestration)
       └─ mpw SC foo.c -o foo.o      (68K C compiler, emulated)
       └─ mpw Link -o app foo.o …    (68K linker, emulated)
       └─ mpw Rez foo.r -o app       (resource compiler, emulated)
```

### Invoking MPW Tools

```
mpw [mpw-flags] <tool> [tool-arguments]
```

**MPW flags** (before the tool name):

| Flag | Purpose |
|------|---------|
| `--ram=<size>` | Emulated RAM (default 16 MB). Supports K/M suffixes |
| `--stack=<size>` | Stack size (default 32 KB) |
| `-D name=value` | Set MPW environment variable |
| `--trace-toolbox` | Log Toolbox trap calls |
| `--trace-mpw` | Log MPW I/O trap calls |
| `--debug` | Launch interactive debugger |

**Tool location:** The emulator searches `$Commands` (comma-separated, set in
`Environment.text`), then `$MPW/Tools/`. The MPW root is found at `$MPW`,
`~/mpw`, `/usr/local/share/mpw`, or `/usr/share/mpw`.

### Key MPW Tools

| Tool | Purpose | Typical invocation |
|------|---------|-------------------|
| `SC` | C compiler (68K) | `mpw SC -p foo.c -o foo.o` |
| `SCpp` | C++ compiler (68K) | `mpw SCpp -p foo.cp -o foo.o` |
| `Asm` | 68K assembler | `mpw Asm foo.a -o foo.o` |
| `Link` | Linker | `mpw Link -o app -c 'APPL' -t APPL foo.o {Libraries}Interface.o …` |
| `Lib` | Librarian (create .o libs) | `mpw Lib -o mylib.o a.o b.o` |
| `Rez` | Resource compiler | `mpw Rez foo.r -o app -append` |
| `DeRez` | Resource decompiler | `mpw DeRez app Types.r` |
| `DumpObj` | Object file inspector | `mpw DumpObj foo.o` |
| `SetFile` | Set Finder type/creator | `mpw SetFile -c 'APPL' -t APPL app` |
| `MrC` | C compiler (PowerPC) | `mpw MrC foo.c -o foo.o` |

### Standard Libraries

68K applications typically link against:

```
{Libraries}Interface.o      # Toolbox glue (trap dispatchers)
{Libraries}Runtime.o        # Runtime startup
{CLibraries}StdCLib.o       # Standard C library
{Libraries}Stubs.o          # Segment loader stubs
{Libraries}ToolLibs.o       # Additional toolbox glue
{CLibraries}CSANELib.o      # Floating-point (if needed)
```

Paths in `{braces}` are MPW environment variables resolved by the emulator.

### Example: Building a Simple 68K Application

```makefile
MPW = mpw
SCFLAGS = -p
LDFLAGS = -w -c 'MPS ' -t MPST \
    -sn STDIO=Main -sn INTENV=Main -sn %A5Init=Main

LIBS = {Libraries}Stubs.o {CLibraries}StdCLib.o \
    {Libraries}Interface.o {Libraries}Runtime.o {Libraries}ToolLibs.o

%.o: %.c
	$(MPW) SC $(SCFLAGS) $< -o $@

myapp: main.o
	$(MPW) Link $(LDFLAGS) -o $@ $^ $(LIBS)
```

## Classic Mac Application Architecture

### Memory Model

Classic Mac apps run in a single flat address space (24-bit or 32-bit addressing).

- **A5 World** — Register A5 points to the application globals boundary.
  Above A5: application global variables. Below A5: the jump table and
  QuickDraw globals.
- **Heap** — Managed by the Memory Manager. Contains handles (relocatable)
  and pointers (non-relocatable).
- **Stack** — Grows downward from the top of the application partition.

**Handles vs. Pointers:**
- `NewHandle(size)` → returns a Handle (double-indirect, relocatable)
- `NewPtr(size)` → returns a Ptr (direct, fixed address)
- Always `HLock()` a handle before dereferencing, `HUnlock()` when done.
- The Memory Manager can move unlocked handles during any trap call.

### CODE Segments

68K executables use CODE resources stored in the resource fork:

- **CODE 0** — Jump table descriptor (A5 world layout)
- **CODE 1+** — Code segments containing 68K machine code

The Segment Loader loads segments on demand. The jump table (below A5)
contains stubs that trap to load unloaded segments.

**Linker flags for segments:**
```
-sn STDIO=Main          # Merge STDIO segment into Main
-sn %A5Init=Main        # Merge A5 init into Main
```

### Resource Forks

Every Mac file can have a data fork and a resource fork. The resource fork
contains typed, numbered resources:

| Type | Purpose |
|------|---------|
| `CODE` | Executable code segments |
| `MENU` | Menu definitions |
| `DLOG` | Dialog templates |
| `DITL` | Dialog item lists |
| `WIND` | Window templates |
| `STR ` | Pascal strings |
| `STR#` | String lists |
| `ICON`, `ICN#`, `cicn` | Icons |
| `PICT` | Pictures |
| `vers` | Version info |
| `SIZE` | Application size/flags |

**Rez source example:**
```
#include "Types.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    multiFinderAware,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    not32BitCompatible,
    reserved,
    reserved,
    reserved,
    reserved,
    reserved,
    reserved,
    reserved,
    393216,     /* preferred memory size */
    393216      /* minimum memory size */
};
```

### Toolbox Traps

Mac OS APIs are invoked via A-line traps (special 68K instructions starting
with 0xA). The trap dispatcher routes these to OS or Toolbox routines.

**Two trap types:**
- **OS traps** (bit 11 = 0): `_NewHandle`, `_NewPtr`, `_Read`, `_Write`, etc.
  Parameters passed in registers.
- **Toolbox traps** (bit 11 = 1): `_GetResource`, `_DrawString`, `_MenuSelect`, etc.
  Parameters passed on the stack (Pascal calling convention).

In C code with MPW headers, you call these as normal functions — the
compiler and Interface.o glue handle the trap mechanism.

### File Manager

Mac files are identified by volume reference number, directory ID, and
filename. The File Manager uses parameter blocks:

```c
HParamBlockRec pb;
pb.ioParam.ioNamePtr = "\pMyFile";    /* Pascal string */
pb.fileParam.ioDirID = dirID;
pb.ioParam.ioVRefNum = vRefNum;
PBHOpenDF(&pb, false);                /* synchronous open */
```

**Path conventions:**
- `:` separates path components (like `/` in Unix)
- Leading `:` means relative path
- `Disk:Folder:File` is an absolute path

### Error Handling

Mac OS routines return `OSErr` (signed 16-bit integer). Zero means success.
Common error codes:

| Code | Name | Meaning |
|------|------|---------|
| 0 | `noErr` | Success |
| -43 | `fnfErr` | File not found |
| -108 | `memFullErr` | Not enough memory |
| -192 | `resNotFound` | Resource not found |
| -39 | `eofErr` | End of file |
| -36 | `ioErr` | I/O error |
| -120 | `dirNFErr` | Directory not found |

### Finder Type and Creator

Every Mac file has a 4-character type and 4-character creator code:

| Type | Creator | Meaning |
|------|---------|---------|
| `APPL` | (varies) | Application |
| `MPST` | `MPS ` | MPW tool (SIOW) |
| `TEXT` | `MPS ` | MPW text file |
| `rsrc` | `RSED` | Resource file |

Set with `mpw SetFile -c 'MPS ' -t MPST myapp` or Link's `-c`/`-t` flags.

## Packaging into HFS Disk Images

Use `package_hfs.py` (installed to `$MPW/bin/`) to package a directory tree
into a raw HFS disk image (.img). The tool reads resource forks and Finder
Info from AppleDouble sidecars or native macOS xattrs.

```
package_hfs.py <directory> [-o output.img] [-n "Volume Name"]
```

**Important:** `package_hfs.py` packages an entire directory tree verbatim.
Structure your build so that the final output lives in a dedicated subdirectory
containing only the files you want on the disk image—no intermediate object
files, build logs, or other artifacts. For example:

```makefile
BUILDDIR = build
DISTDIR  = dist/MyApp

myapp: main.o
	$(MPW) Link $(LDFLAGS) -o $(DISTDIR)/MyApp $^ $(LIBS)
	$(MPW) Rez MyApp.r -o $(DISTDIR)/MyApp -append

disk: myapp
	package_hfs.py $(DISTDIR) -o MyApp.img -n "My App"
```

## Documentation Reference

The following documentation is bundled with this skill. Read these files
when you need detailed API specifications, tool flags, or platform details.

### Inside Macintosh (Toolbox & OS API Reference)

The definitive references for all Mac OS managers and APIs:

- `${CLAUDE_SKILL_DIR}/docs/inside-macintosh/volume-i.md`
  — QuickDraw, Event Manager, Window Manager, Control Manager, Menu Manager,
  TextEdit, Dialog Manager, Resource Manager
- `${CLAUDE_SKILL_DIR}/docs/inside-macintosh/volume-ii.md`
  — Memory Manager, Segment Loader, OS Event Manager, File Manager, Device
  Manager, Printing Manager, SCSI Manager, Sound Driver, Serial Driver
- `${CLAUDE_SKILL_DIR}/docs/inside-macintosh/volume-iii.md`
  — Finder interface, OS Utilities, Package Manager, International Utilities
- `${CLAUDE_SKILL_DIR}/docs/inside-macintosh/volume-iv.md`
  — Mac Plus updates: trap dispatch, Resource Manager extensions, List Manager
- `${CLAUDE_SKILL_DIR}/docs/inside-macintosh/volume-v.md`
  — Mac SE/II: Color QuickDraw, Sound Manager, NuBus, Slot Manager

### MPW Tool References

Compilers, linker, and other MPW tools in detail:

- `${CLAUDE_SKILL_DIR}/docs/mpw/command-reference.md`
  — Complete reference for all MPW commands and built-in tools
- `${CLAUDE_SKILL_DIR}/docs/mpw/building-and-managing.md`
  — How to structure projects, use Make, manage dependencies
- `${CLAUDE_SKILL_DIR}/docs/mpw/introduction.md`
  — Getting started with MPW, shell usage, environment
- `${CLAUDE_SKILL_DIR}/docs/tools/sc-scpp.md`
  — SC/SCpp 68K C/C++ compiler flags, pragmas, extensions
- `${CLAUDE_SKILL_DIR}/docs/tools/mrc-mrcpp.md`
  — MrC/MrCpp PowerPC C/C++ compiler flags and extensions
- `${CLAUDE_SKILL_DIR}/docs/mpw/assembler-reference.md`
  — 68K assembler directives and syntax
- `${CLAUDE_SKILL_DIR}/docs/tools/ppc-assembler.md`
  — PowerPC assembler reference
- `${CLAUDE_SKILL_DIR}/docs/mpw/object-file-format.md`
  — MPW 68K object file format specification
- `${CLAUDE_SKILL_DIR}/docs/mpw/toolserver-reference.md`
  — ToolServer (batch MPW command execution)

### Platform Architecture

- `${CLAUDE_SKILL_DIR}/docs/runtime-architectures.md`
  — CODE segments, jump tables, A5 world, CFM, runtime linking

### Debugger & Resource Tool References

- `${CLAUDE_SKILL_DIR}/docs/tools/macsbug.md`
  — MacsBug debugger reference and debugging guide
- `${CLAUDE_SKILL_DIR}/docs/tools/resedit.md`
  — Resource types, binary layouts, resource editing

### Develop Magazine Articles (Practical MPW Usage)

- `${CLAUDE_SKILL_DIR}/docs/articles/streamedit.md` — Automated editing with StreamEdit
- `${CLAUDE_SKILL_DIR}/docs/articles/environment-config.md` — Building a better Environment.text
- `${CLAUDE_SKILL_DIR}/docs/articles/scripted-text-editing.md` — MPW scripted text editing
- `${CLAUDE_SKILL_DIR}/docs/articles/source-control.md` — Customizing Projector source control
- `${CLAUDE_SKILL_DIR}/docs/articles/launching-faster.md` — Startup optimization
- `${CLAUDE_SKILL_DIR}/docs/articles/speed-software-development.md` — MPW workflow tips
- `${CLAUDE_SKILL_DIR}/docs/articles/toolserver-caveats.md` — ToolServer tips and pitfalls
- `${CLAUDE_SKILL_DIR}/docs/articles/exception-handling.md` — Exception handling in MPW C++

### MPW Community Knowledge

- `${CLAUDE_SKILL_DIR}/docs/mpw-dev-archive.txt`
  — Full MPW-Dev mailing list archive (392 digests). Search this for
  real-world tips, workarounds, and answers to specific tool questions.

### File Format Specifications

- `${CLAUDE_SKILL_DIR}/docs/mpw/sym-file-format.txt` — SYM debug symbol file format
- `${CLAUDE_SKILL_DIR}/docs/mpw/projector-db-format.txt` — Projector source control database format
