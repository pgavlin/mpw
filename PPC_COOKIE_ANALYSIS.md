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

## Open Question

What exactly goes at cookie+0x00 and cookie+0x04? The FIODUPFD path reads `*(cookie+0x00)` and compares to -0x7FF8 (sentinel for "no device"). If it's NOT -0x7FF8, it passes the value to a device resolver. The "not connected" write path reads `*(cookie+0x04)` similarly.

Most likely the cookie data is:
```
+0x00: uint32  — pointer? or 4CC device type tag?
+0x04: C string — device name (e.g., "ECON")
```

This should be verified by checking what `MPW::Init()` creates for the 68K path and what the device resolver function at 0x8EFC expects.
