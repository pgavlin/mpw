# Phase 7: Iteration and Troubleshooting

**Goal:** Diagnose and fix issues that emerge during Phase 6 integration, using the tools we've built.

**Depends on:** Phase 6 (ECON device handlers).

**Status:** COMPLETE — Hello, world! prints correctly with exit status 0.

---

## Diagnostic Toolkit

| Tool | Flag | Usage |
|------|------|-------|
| CFM stub dispatch | `--trace-toolbox` | Shows every stub call with args and return values |
| PEF loader logging | `--trace-toolbox` | Shows section loading, import resolution, relocations |
| PPC instruction trace | `--trace-cpu` | Shows every PPC instruction (very verbose) |
| ECON device trace | `--trace-mpw` | Shows ECON read/write/ioctl calls |
| Interactive PPC debugger | `--debug` | Breakpoints, step, register/memory inspection, PPC disassembly |
| PEF inspector | `pef_inspect` | Structural queries: tocoff, tvec, sym, disasm, imports/exports |
| Unresolved stub handler | (automatic) | Catch-all that logs and halts on unimplemented calls |
| Annotated disassembly | `dump-pef-stdclib`, `dump-pef-hello` | Pre-annotated DumpPEF output for key functions |
| `DumpPEF` | (external) | `mpw DumpPEF -do All -pi u -a -fmt on <path>` |
| `lldb` | (external) | For host-side crashes (mplite, Unicorn, etc.) |

---

## Resolved Issues

### FP Unavailable Exception (RESOLVED)

PPC 603e requires MSR[FP]=1 for floating-point instructions. Without it, `lfd`/`stfd` in longjmp triggered an exception that our interrupt hook misinterpreted as an `sc` call, skipping the rest of longjmp and leaving r3=0 instead of 1.

**Fix:** Set `MSR = 0x2000` during `PPC::Init()`. Committed in `e04aafc5`.

### Exit Path (RESOLVED)

Confirmed via debugger: `exit()` → `_RTExit` → `_DoExitProcs` → dispose routine descriptors → walk exit chain (`info+0x24` → `_IntEnv+0x16` → `__target_for_exit`) → `longjmp(__target_for_exit, 1)` → `__start` cleanup → return. All working correctly.

### Exit Chain Setup (RESOLVED — not needed)

StdCLib's `shared_init_helper` writes `&_IntEnv` into `info+0x24`. `_RTInit` writes `&__target_for_exit` into the cell at `*(_IntEnv+0x16)`. We do NOT need to set up the exit chain externally. See `PPC_STDCLIB_INIT.md`.

### PPC Stack Corrupting mplite Metadata (RESOLVED)

**Root cause:** The PPC stack pointer was initialized to `memorySize - stackSize - 64` (0xFF7FC0), placing it at the boundary between the mplite pool and the stack area. mplite's `aCtrl` metadata array is stored at the tail of the pool buffer (emulated addresses 0xF7C9A0–0xFF7FED). The stack area starts at 0xFF8000, leaving only 19 bytes gap. Any PPC stack frame growth (SP going below 0xFF7FED) immediately corrupted `aCtrl`, causing `mplite_unlink` assertion failures on subsequent `NewPtr` calls.

**Diagnosed using:** `lldb` breakpoint on `__assert_rtn`, then computing:
- `aCtrl = zPool + nBlock * szAtom` (within the emulated memory buffer)
- Pool buffer = `memory + globals` to `memory + memorySize - stack`
- `nBlock = poolSize / (szAtom + 1)` = 505421 (szAtom=32)
- `aCtrl` starts at emulated addr 0xF7C9A0, ends at 0xFF7FED
- Initial PPC SP at 0xFF7FC0 was already 45 bytes inside `aCtrl`

**Fix:** Set PPC SP to `memorySize - 64` (top of the actual stack area, 0xFFFFB0). The stack area is the last `stackSize` bytes of memory and is excluded from the mplite pool. Committed in `adc25708`.

### Handle-Based Cookies (RESOLVED)

`allocateCookieHandle` initially simulated handles via two `NewPtr` calls. Changed to use `MM::Native::NewHandle` which properly registers with the Handle system (needed for StdCLib's `HLock`/`HUnlock` calls on cookies).

### ECON Device Handlers (RESOLVED)

All five ECON handlers work: `econ_read`, `econ_write`, `econ_close`, `econ_ioctl`, `econ_faccess`. StdCLib calls FIODUPFD (3x, one per stdio fd) and FIOBUFSIZE (1x, for stdout) during init. The `_coWrite` path resolves the device handler via `_getIOPort` when the cookie is not connected, and calls `econ_write` via `CallUniversalProc`.

---

## Validation (all passing)

1. `./bin/mpw --ppc tools/Hello` prints "Hello, world!" and exits 0 ✓
2. No crashes, hangs, or infinite loops ✓
3. `--trace-mpw` shows ECON write with the output data ✓
4. 68K tools still work (`DumpPEF`, etc.) ✓
