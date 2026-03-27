# Phase 8: Additional InterfaceLib Wrappers and File I/O for Tools

**Goal:** Implement InterfaceLib wrappers and file I/O support so PPC tools beyond Hello can run.

**Depends on:** Phases 5-7 (Hello, world! runs end-to-end).

**Status:** In progress. 29 InterfaceLib stubs added, FSYS device handlers working, stdio file I/O working. DumpPEF gets through file open/read but crashes during PEF parsing (unmapped memory read).

---

## Completed Work

### InterfaceLib Wrappers (29 new stubs)

All 44 InterfaceLib symbols that DumpPEF imports are now registered. New wrappers were added by reusing existing `Native::` functions:

- **Cursor ops:** ShowCursor, GetCursor, SetCursor (no-ops for CLI)
- **MM extras:** SetHandleSize, HGetState, HSetState, HPurge, HNoPurge, MoveHHi (new `MM::Native::` functions factored out of 68K handlers)
- **RM extras:** HomeResFile, GetResource, Get1Resource, GetResAttrs, GetResInfo, GetResourceSizeOnDisk, SetResLoad, UseResFile, OpenResFile, CloseResFile, UpdateResFile, AddResource, ChangedResource, RemoveResource, ReadPartialResource (new `RM::Native::Get1Resource`, `CreateResFile`, `ReadPartialResource`)
- **File Manager:** PBReadSync, PBHGetFInfoSync
- **Low memory:** LMGetBootDrive, LMGetCurrentA5
- **Misc:** FindFolder (returns fnfErr), numtostring, createresfile, openresfile

### MPW File I/O Refactoring

Factored `ftrap_access/read/write/close/ioctl` into `MPW::Native::` functions shared by 68K and PPC:

| Native Function | Source File | Called By |
|-----------------|------------|-----------|
| `MPW::Native::Access(name, op, parm)` | `mpw/mpw_access.cpp` | `ftrap_access` (68K), `econ_faccess`/`fsys_faccess` (PPC) |
| `MPW::Native::Read(parm)` | `mpw/mpw_io.cpp` | `ftrap_read` (68K), `econ_read`/`fsys_read` (PPC) |
| `MPW::Native::Write(parm)` | `mpw/mpw_io.cpp` | `ftrap_write` (68K), `econ_write`/`fsys_write` (PPC) |
| `MPW::Native::Close(parm)` | `mpw/mpw_close.cpp` | `ftrap_close` (68K), `econ_close`/`fsys_close` (PPC) |
| `MPW::Native::IOCtl(parm, cmd, arg)` | `mpw/mpw_ioctl.cpp` | `ftrap_ioctl` (68K), `econ_ioctl`/`fsys_ioctl` (PPC) |

### FSYS + ECON Device Handlers

Both FSYS and ECON device table entries now have PPC-callable TVectors. The ECON handlers delegate to `MPW::Native::` for file operations (open, read, write, close, ioctl), handling console fds specially for direct host I/O with CR→LF conversion.

Key design: on 68K, FSYS and ECON share the same trap handlers. On PPC, the ECON handlers serve as the unified entry point (since all IO table entries point to ECON), and forward non-console operations to `MPW::Native::`.

### Handle Cookie Wrapping

When `econ_faccess` opens a file via `MPW::Native::Access`, `ftrap_open` writes a raw fd as the cookie. The PPC handler wraps it in a Handle (via `allocateCookieHandle`) and registers it in `cookieFdMap`, matching the convention StdCLib expects. The device pointer (`ioEntry+4`) is also set to the ECON device entry so `_coRead`/`_coWrite` can find handlers via `_getIOPort`.

For Native:: calls that expect raw fd cookies, `patchCookieForNative`/`restoreCookie` temporarily swap the Handle for the raw fd.

---

## Resolved Bugs

### ECON read/write count field (RESOLVED)

The ECON console read/write handlers were writing bytes transferred to `ioEntry+12`, but StdCLib expects remaining bytes (`count - transferred`). This caused accumulated/repeated output. Fixed to match `MPW::Native::Read/Write` convention.

### FIOINTERACTIVE/FIOBUFSIZE for file fds (RESOLVED)

The ECON ioctl handler was hardcoding FIOINTERACTIVE=0 (interactive) and FIOBUFSIZE=2048 for ALL fds. For file fds, these need to go through `MPW::Native::IOCtl` which calls `isatty()` and uses proper buffer logic. Fixed by forwarding all ioctl commands through the Native handler.

### fread returning 0 after fprintf (RESOLVED)

Root cause was the FIOINTERACTIVE bug above — StdCLib treated the file as a terminal and set up line-buffered mode, which interacted badly with buffer initialization. Forwarding to Native::IOCtl fixed this.

---

## Current State: DumpPEF

PPC DumpPEF (`~/mpw/Tools/DumpPEF`) runs without unimplemented stub errors. File I/O works — it opens the resource fork, reads the `cfrg` resource, and opens the data fork. But it crashes during PEF container parsing:

```
PPC: execution error at PC=00F24A04: Invalid memory read (UC_ERR_READ_UNMAPPED)
```

This is an unmapped memory access in StdCLib code (offset 0x14A04 in the code section). Needs investigation — likely a buffer allocation that returned an address outside mapped memory, or a pointer arithmetic overflow.

### FileRead Test Tool

`tools/FileRead` exercises file I/O paths. Confirmed working:
- `open()`/`read()`/`close()` (low-level) ✓
- `fopen()`/`fgetc()`/`fclose()` (stdio) ✓
- `fopen()`/`fread()`/`fclose()` (stdio) ✓
- `fprintf()` to stdout/stderr between fopen and fread ✓

---

## Known Working Infrastructure

| Subsystem | Status |
|-----------|--------|
| PPC CPU (Unicorn) | Working. MSR[FP]=1. |
| PEF loader | Working. pidata decompression, full relocation engine. |
| CFM stubs (sc dispatch) | Working. On-demand library loading. |
| Memory Manager | Working. PPC SP in stack area, not pool area. |
| ECON device handlers | Working. Console + file I/O via Native:: delegation. |
| FSYS device handlers | Working. Direct Native:: calls. |
| Handle-based cookies | Working. Proper NewHandle, cookieFdMap, patchCookieForNative. |
| MPW file I/O (Native::) | Working. Shared between 68K and PPC. |
| CallUniversalProc trampoline | Working. PPC TVector dispatch. |
| Interactive debugger | Working. PPC step/break/registers/disassembly. |
| pef_inspect tool | Working. Structural queries on PEF binaries. |
| 95 InterfaceLib stubs | Working. 66 from Phase 4 + 29 from Phase 8. |

---

## Next Steps

1. Investigate DumpPEF crash at `0xF24A04` (unmapped memory read)
2. Test additional PPC tools (compilers, linkers) and add stubs as needed
