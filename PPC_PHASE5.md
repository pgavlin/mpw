# Phase 5: MPW Environment for PPC

**Goal:** Set up the runtime data structures that StdCLib expects when running under the MPW shell: ECON device table, IO cookies, and the exit/longjmp chain.

**Depends on:** Phase 3 (CFM stubs, for device handler registration), Phase 4 (InterfaceLib wrappers, for CallUniversalProc).

---

## Overview

When StdCLib initializes, it reads the MPGM (MacProgramInfo) block at low memory 0x0316 to discover the MPW shell environment. This includes the device table (how to do I/O), the IO table (stdin/stdout/stderr), and the exit chain (how to return from exit()).

The existing `MPW::Init()` already creates this structure for 68K. For PPC, we need to:
1. Replace 68K F-trap pointers in the device table with PPC-callable TVectors
2. Use pointer-based cookies (not bare fd numbers)
3. Set up info+0x24 for the exit/longjmp chain
4. Optionally set up info+0x28 for the startup entry list

---

## ECON Device Table

### Current 68K Structure

The existing `MPW::Init()` creates a device table with three 24-byte entries (FSYS, ECON, SYST). The FSYS entry has function pointers to inline 68K trap code (e.g., `F001` + `RTS`). ECON and SYST are empty (name only, no handlers).

### PPC Structure

For PPC, all three device entries need PPC-callable function pointers. StdCLib routes console I/O through the ECON device. Each device entry is:

```
+0x00: uint32 name        ('FSYS', 'ECON', 'SYST')
+0x04: uint32 faccess     → TVector for faccess handler
+0x08: uint32 close       → TVector for close handler
+0x0C: uint32 read        → TVector for read handler
+0x10: uint32 write       → TVector for write handler
+0x14: uint32 ioctl       → TVector for ioctl handler
```

### ECON Device Handlers

Register 5 CFM stubs for the ECON device handlers. These are called through `CallUniversalProc` by StdCLib, with arguments shifted:

- **read/write/close**: `CallUniversalProc(handler, 0xF1, ioEntry)` → handler receives `r3=ioEntry`
- **ioctl**: `CallUniversalProc(handler, 0xFF1, ioEntry, cmd, arg)` → handler receives `r3=ioEntry, r4=cmd, r5=arg`
- **faccess**: `CallUniversalProc(handler, 0xFF1, ioEntry, opcode, arg)` → handler receives `r3=ioEntry, r4=opcode, r5=arg`

Each handler reads the ioEntry (20-byte record) and the cookie to determine the fd:

```
ioEntry:
  +0x00: uint16 flags
  +0x02: uint16 error
  +0x04: uint32 device    → device table entry pointer
  +0x08: uint32 cookie    → pointer to per-fd cookie struct
  +0x0C: uint32 count
  +0x10: uint32 buffer
```

**econ_read:**
```cpp
static void econ_read() {
    uint32_t ioEntry = PPC::GetGPR(3);
    uint32_t cookie = memoryReadLong(ioEntry + 8);
    int hostFd = cookieToFd(cookie);
    uint32_t count = memoryReadLong(ioEntry + 12);
    uint32_t buffer = memoryReadLong(ioEntry + 16);

    ssize_t n = ::read(hostFd, memoryPointer(buffer), count);
    if (n < 0) {
        memoryWriteWord(0xFFFF, ioEntry + 2);  // error
        memoryWriteLong(0, ioEntry + 12);       // count = 0
    } else {
        memoryWriteWord(0, ioEntry + 2);        // no error
        memoryWriteLong(n, ioEntry + 12);       // actual count
    }
    PPC::SetGPR(3, n < 0 ? -1 : 0);
}
```

**econ_write:**
```cpp
static void econ_write() {
    uint32_t ioEntry = PPC::GetGPR(3);
    uint32_t cookie = memoryReadLong(ioEntry + 8);
    int hostFd = cookieToFd(cookie);
    uint32_t count = memoryReadLong(ioEntry + 12);
    uint32_t buffer = memoryReadLong(ioEntry + 16);

    // CR→LF conversion for text mode
    uint8_t *ptr = memoryPointer(buffer);
    std::vector<uint8_t> converted;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t c = ptr[i];
        converted.push_back(c == '\r' ? '\n' : c);
    }

    ssize_t n = ::write(hostFd, converted.data(), converted.size());
    memoryWriteWord(n < 0 ? 0xFFFF : 0, ioEntry + 2);
    memoryWriteLong(n < 0 ? 0 : (uint32_t)n, ioEntry + 12);
    PPC::SetGPR(3, n < 0 ? -1 : 0);
}
```

**econ_close:**
```cpp
static void econ_close() {
    // No-op for console fds (don't close stdin/stdout/stderr)
    PPC::SetGPR(3, 0);
}
```

**econ_ioctl:**
```cpp
static void econ_ioctl() {
    uint32_t ioEntry = PPC::GetGPR(3);
    uint32_t cmd = PPC::GetGPR(4);
    uint32_t arg = PPC::GetGPR(5);

    switch (cmd) {
    case 0x6601: // FIODUPFD
        // Register the cookie→fd mapping
        {
            uint32_t cookie = memoryReadLong(ioEntry + 8);
            // The arg is the host fd to associate
            // Actually, FIODUPFD in MPW duplicates the file descriptor.
            // For console, we just register the cookie→fd association.
            registerCookieFd(cookie, hostFdFromIoEntry(ioEntry));
        }
        PPC::SetGPR(3, 0);
        break;

    case 0x6602: // FIOINTERACTIVE
        // Return 0 = "this fd is interactive" (a terminal/console)
        // This is CRITICAL: StdCLib uses this to decide whether to
        // set the line-buffer flag on stdout's FILE structure.
        PPC::SetGPR(3, 0);
        break;

    case 0x6603: // FIOBUFSIZE
        // Return 0 (success), write buffer size to *arg
        if (arg) memoryWriteLong(2048, arg);
        PPC::SetGPR(3, 0);
        break;

    case 0x6605: // FIOREFNUM
        // Return -1 (no Mac refNum for console)
        PPC::SetGPR(3, (uint32_t)(int32_t)-1);
        break;

    default:
        fprintf(stderr, "ECON ioctl: unknown cmd 0x%04X\n", cmd);
        PPC::SetGPR(3, (uint32_t)(int32_t)-1);
        break;
    }
}
```

**econ_faccess:**
```cpp
static void econ_faccess() {
    // faccess(ioEntry, opcode, arg)
    PPC::SetGPR(3, 0);  // success
}
```

### ECON Trace Logging

All ECON handlers should include trace output (gated behind the existing `--trace-mpw` flag, since ECON device I/O is the PPC equivalent of MPW F-trap I/O). ECON calls are the most important thing to trace during StdCLib init — they reveal whether stdout is being set up correctly:

```cpp
static void econ_ioctl() {
    uint32_t ioEntry = PPC::GetGPR(3);
    uint32_t cmd = PPC::GetGPR(4);
    uint32_t arg = PPC::GetGPR(5);
    uint32_t cookie = memoryReadLong(ioEntry + 8);
    int fd = cookieToFd(cookie);

    static const char *cmdNames[] = { "?", "FIODUPFD", "FIOINTERACTIVE",
                                       "FIOBUFSIZE", "?", "FIOREFNUM" };
    const char *cmdName = (cmd >= 0x6601 && cmd <= 0x6605)
                          ? cmdNames[cmd - 0x6600] : "unknown";

    if (Trace) {
        fprintf(stderr, "  ECON ioctl(fd=%d, %s/0x%04X, arg=0x%08X)\n",
                fd, cmdName, cmd, arg);
    }

    // ... dispatch by cmd ...

    if (Trace) {
        fprintf(stderr, "    -> %d", (int32_t)PPC::GetGPR(3));
        if (cmd == 0x6603 && arg) // FIOBUFSIZE
            fprintf(stderr, " (*arg=%d)", memoryReadLong(arg));
        fprintf(stderr, "\n");
    }
}
```

The expected trace during StdCLib init:
```
  ECON ioctl(fd=0, FIODUPFD/0x6601, arg=0x...)    -> 0
  ECON ioctl(fd=1, FIODUPFD/0x6601, arg=0x...)    -> 0
  ECON ioctl(fd=2, FIODUPFD/0x6601, arg=0x...)    -> 0
  ECON ioctl(fd=1, FIOBUFSIZE/0x6603, arg=0x...)  -> 0 (*arg=2048)
  ECON ioctl(fd=1, FIOINTERACTIVE/0x6602, arg=0x...) -> 0
  ECON ioctl(fd=1, FIOREFNUM/0x6605, arg=0x...)   -> -1
```

If these calls don't appear, StdCLib isn't finding or using the ECON device — check the device table patching.

Similarly, `econ_write` should trace the data being written:
```cpp
if (Trace) {
    fprintf(stderr, "  ECON write(fd=%d, count=%d, buf=\"%.*s\")\n",
            fd, count, std::min(count, 40u), (char *)memoryPointer(buffer));
}
```

### Cookie Structure

Cookies are pointers to small per-fd state structures. StdCLib inspects the cookie to determine code paths. The prior work found that the cookie has at least:

```
cookie+0x00: uint8_t mode (0x01=read, 0x02=write, 0x03=read+write)
cookie+0x01-0x0B: reserved/padding
cookie+0x0C: uint8_t connected (1 = device is connected)
```

We allocate one cookie struct per fd (stdin, stdout, stderr) in emulated memory and maintain a host-side `std::map<uint32_t, int>` mapping cookie address → host fd number.

```cpp
// Allocate cookies
uint32_t stdinCookie = allocateCookie(0x01, 0);   // mode=read, fd=0
uint32_t stdoutCookie = allocateCookie(0x02, 1);   // mode=write, fd=1
uint32_t stderrCookie = allocateCookie(0x02, 2);   // mode=write, fd=2
```

---

## IO Table

Three 20-byte ioEntry records. For PPC, the cookie field points to the cookie struct (not a bare fd number), and the device field points to the ECON device table entry:

```cpp
// stdout ioEntry
ptr = ioBase + 20;
memoryWriteWord(0x0002, ptr + 0);     // flags: write
memoryWriteWord(0x0000, ptr + 2);     // error: none
memoryWriteLong(econDevPtr, ptr + 4); // device: ECON entry
memoryWriteLong(stdoutCookie, ptr + 8); // cookie: pointer to cookie struct
memoryWriteLong(0, ptr + 12);          // count
memoryWriteLong(0, ptr + 16);          // buffer
```

---

## Exit Chain (info+0x24)

### Background

When a PPC MPW tool calls `exit(n)`:
1. `exit()` calls `_RTExit`
2. `_RTExit` checks the StandAlone flag (RTOC+0x1BD0)
3. If StandAlone == 0 (under MPW shell): stores exit code, then walks a pointer chain to find a jmp_buf and calls `longjmp(jmpbuf, 1)`
4. `longjmp` returns to the tool's `__start` function, which had called `setjmp(__target_for_exit)` at the beginning

The pointer chain from the findings document:
```
p1 = *(info + 0x24)     // exit chain pointer
p2 = *(p1 + 0x16)       // pointer to a pointer to jmp_buf
jmpbuf = *p2             // the jmp_buf address
longjmp(jmpbuf, 1)
```

### Setup

After loading StdCLib, look up the `__target_for_exit` export. This is a data symbol in StdCLib's data section where `__start` stores its jmp_buf via `setjmp()`.

```cpp
uint32_t targetForExit = PEFLoader::FindExport(stdclibResult, "__target_for_exit");
```

Then allocate the chain structures:

```cpp
// Allocate a "pointer slot" that holds the address of __target_for_exit
uint32_t ptrSlot;
MM::Native::NewPtr(4, true, ptrSlot);
memoryWriteLong(targetForExit, ptrSlot);

// Allocate a "chain node" (at least 0x1A bytes)
// At offset +0x16, it holds the address of the pointer slot
uint32_t chainNode;
MM::Native::NewPtr(0x1A, true, chainNode);
memoryWriteLong(ptrSlot, chainNode + 0x16);

// Set info+0x24 to point to the chain node
memoryWriteLong(chainNode, MacProgramInfo + 0x24);
```

**Important caveat**: This chain layout is based on the findings document and needs runtime verification. The actual chain walk in `_RTExit` may differ. If exit crashes, Phase 7 debugging will trace the exact dereference sequence.

---

## Startup Entry List (info+0x28)

StdCLib's init helper walks `info+0x28` looking for tagged entries:
- `'getv'`: get environment variable callback
- `'setv'`: set environment variable callback
- `'syst'`: system info callback
- `'strt'`: startup callback

The entry list format is an array of `{uint32_t tag, uint32_t handler}` pairs, terminated by `{0, 0}`.

**Start with NULL (info+0x28 = 0)**. StdCLib's init should proceed without these — it falls back to standalone behavior for environment variables. If testing reveals that the init fails without 'getv'/'setv', add them:

```cpp
// Only if needed:
uint32_t startupList;
MM::Native::NewPtr(3 * 8, true, startupList);  // 2 entries + terminator
memoryWriteLong('getv', startupList + 0);
memoryWriteLong(getvStubTVec, startupList + 4);
memoryWriteLong('setv', startupList + 8);
memoryWriteLong(setvStubTVec, startupList + 12);
memoryWriteLong(0, startupList + 16);  // terminator
memoryWriteLong(0, startupList + 20);
memoryWriteLong(startupList, MacProgramInfo + 0x28);
```

---

## Files to Create/Modify

### New: ECON handlers in `toolbox/ppc_dispatch.cpp`

Add the ECON handler functions and a `PatchDeviceTable()` function that replaces the 68K device table entries with PPC TVectors:

```cpp
namespace PPCDispatch {
    void PatchDeviceTable(uint32_t devTablePtr);
}
```

This is called from the PPC loading path in `loader.cpp` after `MPW::Init()` has created the base device table.

### Modify: `mpw/mpw.cpp`

Extend `MPW::Init()` to:
1. Allocate pointer-based cookies instead of bare fd numbers in the IO table
2. Point IO entries to the ECON device instead of FSYS
3. Export `MacProgramInfo` address so the loader can set info+0x24/info+0x28

Or create a new `MPW::InitPPC()` that builds the entire MPGM block with PPC-specific settings from scratch.

### Modify: `mpw/mpw.h`

Add:
```cpp
namespace MPW {
    uint32_t GetMacProgramInfo();  // returns info block address
    // Or expose MacProgramInfo directly
}
```

---

## Implementation Steps

1. Add ECON handler functions to `toolbox/ppc_dispatch.cpp`:
   a. `econ_read()`, `econ_write()`, `econ_close()`, `econ_ioctl()`, `econ_faccess()`
   b. Cookie allocation helper
   c. Cookie→fd mapping (host-side `std::map`)
   d. `PatchDeviceTable()` — register ECON stubs, write TVectors into device table
2. Modify `mpw/mpw.cpp`:
   a. Add cookie allocation to IO table setup (or create `InitPPC()`)
   b. Point IO entries at ECON device
   c. Expose MacProgramInfo address
3. Add exit chain setup (in loader.cpp or a helper):
   a. Look up `__target_for_exit` from StdCLib exports
   b. Allocate chain node and pointer slot
   c. Write info+0x24

---

## Validation

### ECON ioctl test

After StdCLib init, verify via tracing that the following ioctl calls were made:
- 3x FIODUPFD (0x6601) — for stdin, stdout, stderr
- 1x FIOBUFSIZE (0x6603) — for stdout
- 1x FIOINTERACTIVE (0x6602) — for stdout
- 1x FIOREFNUM (0x6605) — for stdout

All should return 0 (success) except FIOREFNUM which returns -1.

### stdout line-buffering test

After StdCLib init completes, inspect stdout's FILE structure in emulated memory. The FILE structure (24 bytes) at the address StdCLib uses for stdout should have the line-buffer flag set in the flags field at offset +0x12. The exact bit for line-buffering needs to be verified (suspected 0x0040 or 0x0010).

If the flag is NOT set despite FIOINTERACTIVE returning 0, the cookie structure may need adjustment — StdCLib may inspect cookie bytes to determine the code path.

### Exit chain test

After setting up info+0x24 and calling StdCLib's `__initialize`:
1. Call a trivial PPC function that calls `exit(0)`
2. Verify that `_RTExit` successfully calls `longjmp` and control returns to the tool's `__start`
3. Verify the exit code is written to info+0x0E

If exit crashes, Phase 7 debugging will trace the exact chain walk.

### ECON write test

Write test: manually invoke `econ_write` with a buffer containing "test\r" and verify "test\n" appears on the host stdout.

---

## Risk Notes

- **Cookie structure layout**: The exact byte layout StdCLib expects in cookies is partially reverse-engineered. If StdCLib inspects cookie bytes we don't set correctly, the code path to FIOINTERACTIVE may be skipped, and stdout won't be line-buffered. Debugging this requires tracing StdCLib's init code.
- **Exit chain layout**: The findings document's chain description (`info+0x24 → +0x16 → *ptr = jmpbuf`) may be incomplete or incorrect. Runtime tracing of `_RTExit` is the definitive way to determine the layout.
- **ECON vs FSYS**: StdCLib routes console I/O through ECON, not FSYS. The IO table entries must point to the ECON device, not FSYS. Getting this wrong means writes go through the wrong handler.
- **CR→LF conversion**: Mac uses CR (0x0D) as line ending; Unix uses LF (0x0A). The ECON write handler must convert. But only for text mode — binary mode should pass through unchanged.
