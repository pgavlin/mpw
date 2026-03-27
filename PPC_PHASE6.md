# Phase 6: MPW Environment for PPC

**Goal:** Implement Handle-based cookies and ECON device handlers so StdCLib's stdio reaches the host, and wire up exit code capture.

**Depends on:** Phase 5 (integration harness). Hello World already runs to completion and exits cleanly (status 0) but produces no output.

---

## Current State

Verified via `--debug`, `--trace-toolbox`, and `--trace-cpu`:
- StdCLib `__initialize` completes successfully (StandAlone=0, MPGM validated)
- StdCLib copies 5 device table entries and the IO table from MPGM into `_IntEnv`
- `__start` calls `setjmp`, `main`, `fprintf`, `exit`
- `_RTExit` runs, calls `_DoExitProcs`, walks exit chain, calls `longjmp`
- `longjmp` returns to `__start` with r3=1, cleanup path reads `_exit_status`, returns
- Tool exits cleanly with status 0

**Root cause of no output (validated via debugger):**
- `MPW::Init()` writes bare fd numbers (0, 1, 2) as cookies in the IO table
- StdCLib treats cookies as Memory Manager Handles: calls `HLock(1)` → invalid Handle
- `*(1)+0xC` reads garbage from low memory → connected flag appears as 0
- With connected=0, `_coIoctl` never reaches FIOINTERACTIVE via device handler
- FIOINTERACTIVE is handled directly (returns 0) but only when called
- **`_coIoctl` is never called at all** — the FILE init allocates a buffer internally without calling any ioctl
- stdout is NOT line-buffered → `\r` doesn't trigger flush
- `_coWrite` is never called — fprintf buffers 14 chars and returns
- `_RTExit` doesn't call `fflush` → buffer is abandoned
- No output reaches the host

What we do NOT need to set up (StdCLib handles these itself):
- `info+0x24` (exit chain) — StdCLib writes `&_IntEnv` there during `shared_init_helper`
- `_IntEnv+0x16` (__target_for_exit pointer) — `_RTInit` writes this
- `info+0x28` (startup entry list) — NULL works fine

---

## What's Needed

### 1. Handle-Based Cookies (Critical Fix)

The IO table cookie field must be a Memory Manager Handle, not a bare fd number. StdCLib does:
```
HLock(cookie)
cookie_data = *(*cookie)     ; double-dereference: Handle → master ptr → data
connected = cookie_data+0x0C ; byte: 0=not connected, non-zero=connected
```

Cookie data layout (from disassembly of `_coIoctl`, `_coWrite`, `_getIOPort`):
```
+0x00: int32  fd index (used by FIODUPFD via _getIOPort)
+0x04: int32  fd index (used by _coWrite via _getIOPort)
+0x0C: uint8  connected flag (0 = not connected initially)
```

When not connected, `_coIoctl` handles FIOINTERACTIVE directly (returns 0 = interactive) and FIODUPFD resolves the fd via `_getIOPort`. When connected, calls go through `CallUniversalProc` to device handler TVectors.

For each stdio fd, allocate:
```cpp
// Allocate cookie data block
uint32_t cookieData;
MM::Native::NewPtr(0x10, true, cookieData);
memoryWriteLong(fdIndex, cookieData + 0x00);  // fd index for FIODUPFD
memoryWriteLong(fdIndex, cookieData + 0x04);  // fd index for _coWrite
memoryWriteByte(0, cookieData + 0x0C);         // not connected initially

// Wrap in a Handle
uint32_t cookieHandle;
MM::Native::NewHandle(4, false, cookieHandle);
// *Handle = pointer to cookie data
uint32_t masterPtr = memoryReadLong(cookieHandle);
memoryWriteLong(cookieData, masterPtr);

// Write Handle into IO table
memoryWriteLong(cookieHandle, ioEntryAddr + 8);
```

### 2. ECON Device Table Entries

The MPGM device table's ECON entry (at devTable+24) needs PPC-callable TVectors for 5 handlers. Register sc stubs for each and write their TVector addresses into the device entry.

When StdCLib's FIODUPFD resolves the fd via `_getIOPort`, it finds the ioEntry, reads the device pointer from ioEntry+4, and calls the device's ioctl handler via `CallUniversalProc`. This is how the cookie gets "connected" to the ECON device.

The IO table entries must also point at the ECON device entry (ioEntry+4 = devTable+24) instead of FSYS (devTable+0).

### 3. ECON Handlers

Five sc stub handlers:

**econ_write**: Read ioEntry (r3 after CallUniversalProc arg shifting), get cookie→fd mapping, read buffer from emulated memory, convert CR→LF, write to host fd.

**econ_read**: Read ioEntry, read from host fd into emulated buffer.

**econ_close**: No-op for console fds.

**econ_ioctl**: Dispatch by command:
- FIODUPFD (0x6601): register cookie→fd mapping, set connected flag
- FIOINTERACTIVE (0x6602): return 0 (interactive)
- FIOBUFSIZE (0x6603): return 0, write 2048 to *arg
- FIOREFNUM (0x6605): return -1

**econ_faccess**: Return 0.

### 4. Exit Code Capture

Add to `RunPPC()` after tool returns:
```cpp
uint32_t rv = MPW::ExitStatus();
if (rv > 0xff) rv = 0xff;
exit(rv);
```

### 5. Trace Logging

Gate ECON handler trace behind `--trace-mpw`.

---

## Expected Call Sequence With Fix

With proper Handle-based cookies and ECON device handlers:

1. **StdCLib init**: copies device table (with ECON TVectors) and IO table (with Handle cookies)
2. **First fprintf to stdout**: FILE has no buffer → calls buffer init function
3. **Buffer init**: calls `_coIoctl(FIOINTERACTIVE)` on stdout's cookie
4. `_coIoctl`: HLock(cookie) → cookie_data+0x0C = 0 (not connected) → handles FIOINTERACTIVE directly → returns 0 (interactive)
5. **Buffer init**: sets line-buffer flag on stdout FILE flags
6. `_coIoctl(FIOBUFSIZE)`: handled directly or via device → returns 0, *arg=2048
7. **fprintf writes**: "Hello, world!\r" into FILE buffer
8. **`\r` triggers line-buffer flush** → calls `_coWrite`
9. `_coWrite`: HLock(cookie) → cookie_data+0x0C = 0 (not connected) → resolves via `_getIOPort(&cookie_data[4])` → finds ioEntry → reads device+0x10 (write handler TVector) → `CallUniversalProc(writeHandler, 0xF1, ioEntry)`
10. **ECON write handler**: reads buffer from emulated memory, CR→LF converts, writes to host stdout
11. **"Hello, world!\n" appears on terminal**

Note: steps 4-6 may or may not happen on the first write — the exact trigger for FIOINTERACTIVE needs runtime verification. The key fix is the Handle-based cookies that make `_getIOPort` work.

---

## Files to Create/Modify

| File | Changes |
|------|---------|
| `toolbox/ppc_dispatch.h` | Add `PatchDeviceTable()` |
| `toolbox/ppc_dispatch.cpp` | Add ECON handlers, cookie Handle allocation, PatchDeviceTable |
| `bin/loader.cpp` | Call PatchDeviceTable after MPW::Init, add exit code capture |

No changes to `mpw/mpw.cpp` or `mpw/mpw.h`.

---

## Validation

```bash
./bin/mpw --ppc tools/Hello
```
Expected: `Hello, world!` on stdout, exit status 0.

```bash
./bin/mpw --ppc --trace-mpw tools/Hello
```
Expected: ECON ioctl/write trace during execution.

```bash
./bin/mpw --ppc --debug tools/Hello
] b 0xF1CC7C        # break at _coWrite
] c                  # should hit on fprintf flush
] p r3               # ioEntry pointer
```

68K regression: `./bin/mpw DumpPEF` still works.
