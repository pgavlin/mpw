# Phase 6: MPW Environment for PPC

**Goal:** Implement ECON device handlers so StdCLib's stdio reaches the host, and wire up exit code capture.

**Depends on:** Phase 5 (integration harness). Hello World already runs to completion and exits cleanly (status 0) but produces no output because StdCLib's internal buffer is never flushed to the host.

---

## Current State

Verified via `--debug` and `--trace-toolbox`:
- StdCLib `__initialize` completes successfully (StandAlone=0, MPGM validated)
- StdCLib copies 5 device table entries and the IO table from MPGM into `_IntEnv`
- `__start` calls `setjmp`, `main`, `fprintf` (buffers internally), `exit`
- `_RTExit` runs, calls `_DoExitProcs`, walks exit chain, calls `longjmp`
- `longjmp` returns to `__start` with r3=1, cleanup path reads `_exit_status`, `blr` returns
- Tool exits cleanly. **No output because ECON device handlers don't exist yet.**

What we do NOT need to set up (StdCLib handles these itself):
- `info+0x24` (exit chain) — StdCLib writes `&_IntEnv` there during `shared_init_helper`
- `_IntEnv+0x16` (__target_for_exit pointer) — `_RTInit` writes this
- `info+0x28` (startup entry list) — NULL works fine, StdCLib falls back

---

## What's Needed

### 1. ECON Device Table Entries

The existing `MPW::Init()` creates a device table with FSYS entries using inline 68K F-trap code. ECON and SYST entries have names but no handlers. For PPC, the ECON entry needs PPC-callable TVectors pointing to sc stubs.

Each device table entry is 24 bytes:
```
+0x00: uint32 name ('FSYS', 'ECON', 'SYST')
+0x04: uint32 faccess handler
+0x08: uint32 close handler
+0x0C: uint32 read handler
+0x10: uint32 write handler
+0x14: uint32 ioctl handler
```

StdCLib copies all 5 entries (120 bytes) into its own `_IntEnv+0x20` during init. The device handlers are called through `CallUniversalProc` by StdCLib with args shifted (r5→r3, r6→r4, etc.).

Add `PPCDispatch::PatchDeviceTable(uint32_t devTablePtr)` that registers 5 ECON handler sc stubs and writes their TVector addresses into the ECON device table entry at `devTablePtr + 24`.

### 2. ECON Handlers

Five handlers registered as CFM stubs:

**econ_write**: Read ioEntry (r3), get cookie→fd mapping, read buffer from emulated memory, convert CR→LF, write to host fd.

**econ_read**: Read ioEntry, read from host fd into emulated buffer.

**econ_close**: No-op for console fds.

**econ_ioctl**: Dispatch by command:
- FIODUPFD (0x6601): register cookie→fd mapping
- FIOINTERACTIVE (0x6602): return 0 (interactive) — critical for line-buffering
- FIOBUFSIZE (0x6603): return 0, write 2048 to *arg
- FIOREFNUM (0x6605): return -1

**econ_faccess**: Return 0 (success).

### 3. Cookie-Based IO Table

The existing `MPW::Init()` writes bare fd numbers (0, 1, 2) as cookies in the IO table. StdCLib expects pointer-based cookies. After `MPW::Init()`, patch the IO table:
- Allocate cookie structs (mode byte + connected flag) for stdin/stdout/stderr
- Write cookie pointers into the IO table entries
- Point IO entries at the ECON device entry (not FSYS)
- Maintain a host-side `map<uint32_t, int>` for cookie→fd mapping

### 4. Exit Code Capture

Add to `RunPPC()` after the tool returns:
```cpp
uint32_t rv = MPW::ExitStatus();  // reads info+0x0E
if (rv > 0xff) rv = 0xff;
exit(rv);
```

### 5. Trace Logging

Gate ECON handler trace output behind `--trace-mpw` (the existing MPW trace flag). Show fd, command name, args, and return value for ioctls; show fd, count, and buffer preview for writes.

---

## Files to Create/Modify

| File | Changes |
|------|---------|
| `toolbox/ppc_dispatch.h` | Add `PatchDeviceTable()` |
| `toolbox/ppc_dispatch.cpp` | Add ECON handlers, cookie management, PatchDeviceTable |
| `bin/loader.cpp` | Call PatchDeviceTable after MPW::Init, add exit code capture |

No changes needed to `mpw/mpw.cpp` or `mpw/mpw.h` — we post-patch the MPGM structures from `RunPPC()`.

---

## Validation

```bash
./bin/mpw --ppc tools/Hello
```
Expected: `Hello, world!` on stdout, exit status 0.

```bash
./bin/mpw --ppc --trace-mpw tools/Hello
```
Expected: ECON ioctl trace during init, ECON write trace during fprintf flush.

```bash
./bin/mpw --ppc --debug tools/Hello
] b 0xF7C27C    # break at setjmp check
] c              # first hit: r3=0 (initial)
] c              # second hit: r3=1 (longjmp return)
] p r3           # verify r3=1
```

68K regression: `./bin/mpw DumpPEF` still works.
