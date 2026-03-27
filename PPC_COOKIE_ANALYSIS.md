# Cookie Structure Analysis

Traced via `pef_inspect` disassembly of StdCLib's `_coIoctl` (0xCD94) and `_coWrite` (0xCC7C).

## Key Finding: Cookies are Handles, Not Pointers

The cookie field in ioEntry+0x08 is a **Memory Manager Handle** (not a bare pointer, not a bare fd number).

StdCLib's code does:
```
r3 = ioEntry+8           ; cookie = Handle
HLock(cookie)             ; lock the handle
r12 = ioEntry+8           ; reload
r9 = *(r12)               ; r9 = *Handle = master pointer
r0 = byte at r9+0xC       ; connected flag
```

## Cookie Data Layout (at *Handle)

```
+0x00: uint32  first word — device name pointer or sentinel value (-0x7FF8 = 0xFFFF8008)
+0x04: varies  device name string (C string, e.g., "ECON\0") — used by device resolver
+0x0C: uint8   connected flag (0 = not connected, non-zero = connected)
```

## Behavior by Connected State

### Not Connected (byte +0xC == 0)

`_coIoctl` handles commands directly:
- FIODUPFD (0x6601): looks up device name at cookie+0x04, resolves against device table, calls device handler's ioctl with FIODUPFD. If successful, presumably sets connected flag.
- FIOINTERACTIVE (0x6602): returns 0 directly (yes, interactive)
- Other commands: returns EINVAL (22)

`_coWrite` when not connected:
- Reads word at cookie+0x04, checks for sentinel -0x7FF8
- If not sentinel: passes cookie+0x04 (device name) to device resolver → finds device → calls device write handler via CallUniversalProc(handler, 0xF1, ioEntry)

### Connected (byte +0xC != 0)

Both `_coIoctl` and `_coWrite`:
- Read device table entry from ioEntry+0x04
- Call handler via CallUniversalProc with appropriate procInfo

## Current Bug

`MPW::Init()` writes bare fd numbers (0, 1, 2) as cookies. StdCLib calls `HLock(1)` on stdout's cookie — invalid Handle. This either crashes silently or returns garbage.

## Required Fix

For each stdio fd, allocate a Handle with the cookie data structure:

```cpp
// Allocate cookie data (at least 0x10 bytes)
uint32_t cookieData;
MM::Native::NewPtr(0x10, true, cookieData);

// Write device name "ECON" at +0x00 (as a 4CC? or C string at +0x04?)
// The exact format needs verification — the code checks *(cookie+0x00)
// against -0x7FF8 and passes cookie+0x04 to the device resolver.
// Most likely: +0x00 is a 4CC tag, +0x04 is a C string device name.

// Write connected flag
memoryWriteByte(0, cookieData + 0x0C);  // 0 = not connected initially

// Wrap in a Handle
uint32_t cookieHandle;
MM::Native::NewHandle(4, false, cookieHandle);
memoryWriteLong(cookieData, cookieHandle);  // *Handle = pointer to data

// Write Handle into IO table
memoryWriteLong(cookieHandle, ioEntry + 8);
```

## Resolved: Cookie Data Layout

Traced via `_getIOPort` (0x973C) disassembly and `--trace-cpu` validation:

```
+0x00: int32  — fd index (used by FIODUPFD path via _getIOPort)
                 or -0x7FF8 (0xFFFF8008) sentinel = "no device assigned"
+0x04: int32  — fd index (used by _coWrite path via _getIOPort)
                 or -0x7FF8 sentinel
+0x0C: uint8  — connected flag (0 = not connected, non-zero = connected)
```

`_getIOPort` reads the fd index, multiplies by 20 (ioEntry size), indexes into the IO table at `_IntEnv+0x1C`, validates that the entry's flags are non-zero (fd is open), and returns the ioEntry pointer.

## Validated via --trace-cpu

- `_coIoctl` and `_coWrite` are NEVER called during Hello execution
- StdCLib's IO init (section offset 0xA908) wraps ECON handlers in NewRoutineDescriptor but does NOT call FIOINTERACTIVE/FIOBUFSIZE during init
- fprintf silently fails: the bare fd cookie (value 1) is not a valid Handle, HLock(1) fails, `*(1)+0xC` reads garbage, `_coWrite` either takes a wrong path or returns EBADF
- No output is produced because the FILE buffer is never flushed to the host

## Fix

Allocate a Handle for each stdio cookie. The Handle's master pointer should point to a cookie data block with:
- `+0x00`: fd index (0 for stdin, 1 for stdout, 2 for stderr)
- `+0x04`: fd index (same value)
- `+0x0C`: 0 (not connected initially — StdCLib will connect via FIODUPFD)
