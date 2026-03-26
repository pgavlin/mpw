# Phase 7: Iteration and Troubleshooting

**Goal:** Diagnose and fix issues that emerge during integration testing, using the trace and debugging tools built into Phases 1-6.

**Depends on:** Phase 6 (end-to-end integration).

---

## Overview

Several aspects of the implementation are based on reverse-engineering findings that may be incomplete. This phase is a troubleshooting guide — not new feature work. All diagnostic tools (`--trace-ppc`, `--trace-ppc-cpu`, `--trace-ppc-loader`, memory watchpoints) were built in earlier phases and are ready to use here.

---

## Diagnostic Toolkit Summary

All PPC trace features reuse existing CLI flags — no new flags were added.

| Tool | Flag | Added in | Usage |
|------|------|----------|-------|
| CFM stub dispatch | `--trace-toolbox` | Phase 3 | Shows every stub call with args and return values |
| PEF loader logging | `--trace-toolbox` | Phase 2 | Shows section loading, import resolution, relocations |
| PPC instruction trace | `--trace-cpu` | Phase 1 | Shows every PPC instruction executed (very verbose) |
| ECON device trace | `--trace-mpw` | Phase 6 | Shows ECON read/write/ioctl calls |
| Interactive debugger | `--debug` | Phase 5 | Breakpoints, register display, memory inspection |
| Memory watchpoints | (programmatic) | Phase 5 | Fires on writes to specific addresses (stdout flags, exit code) |
| Unresolved stub handler | (automatic) | Phase 4 | Catch-all that logs and halts on unimplemented calls |
| `DumpPEF` | (external tool) | — | `mpw DumpPEF -do All -pi u -a -fmt on <path>` for full PEF disassembly |

---

## Likely Issue 1: Missing InterfaceLib Stubs

**Symptom:** StdCLib init crashes. `--trace-toolbox` shows the last successful stub call, then the catch-all handler fires with "unimplemented stub" message.

**Diagnosis:**
1. Run with `--trace-toolbox`
2. The catch-all handler (from Phase 4) prints: `PPC FATAL: unimplemented stub InterfaceLib::SomeFunction called`
3. Check register dump in the error message for argument values

**Fix:** Add the missing stub to `ppc_dispatch.cpp`. Most are simple wrappers:
```cpp
reg("InterfaceLib", "SomeFunction", []() {
    PPC::SetGPR(3, 0);  // return 0 / noErr
});
```

---

## Likely Issue 2: stdout Not Line-Buffered

**Symptom:** Tool runs and exits without output. Or output appears only with explicit `fflush()`.

**Diagnosis:**
1. Check `--trace-mpw` output for ECON ioctl calls during StdCLib init:
   ```
   ECON ioctl(fd=1, FIOINTERACTIVE/0x6602, arg=...) -> 0
   ECON ioctl(fd=1, FIOBUFSIZE/0x6603, arg=...)     -> 0 (*arg=2048)
   ```
   If these lines are missing, StdCLib isn't reaching the ECON device path — check device table patching.

2. After StdCLib init, inspect stdout's FILE flags. StdCLib exports `stdout` as a data symbol:
   ```cpp
   uint32_t stdoutFile = PEFLoader::FindExport(stdclibResult, "stdout");
   uint16_t flags = memoryReadWord(stdoutFile + 0x12);
   fprintf(stderr, "stdout flags: 0x%04X\n", flags);
   ```

3. If FIOINTERACTIVE is called but flags don't include line-buffer bit, use `--trace-cpu` to trace StdCLib's init code from the ioctl return through the flag-setting logic. Cross-reference with `DumpPEF` disassembly.

**Possible fixes:**
- Cookie `connected` flag (byte +0x0C) must be 1. StdCLib may skip FIOINTERACTIVE if not connected.
- Cookie `mode` byte (+0x00) must match fd direction (0x02 for stdout).
- FIOINTERACTIVE return value must be in r3 (0 = interactive), not only in the ioEntry error field.

**Last resort:** Manually patch the FILE flags after init — this is a data structure fix, not a function intercept.

---

## Likely Issue 3: Exit Chain Layout Wrong

**Symptom:** `exit(0)` crashes. `--trace-cpu` shows `_RTExit` (StdCLib offset 0xF050) reading garbage pointers.

**Diagnosis:**
1. Run with `--trace-cpu` and look for instructions around `_RTExit` (absolute address = `stdclibResult.sections[0].address + 0xF050`)
2. Use memory watchpoints on `info+0x24` to see exactly when and how `_RTExit` reads the chain
3. Trace the dereference chain:
   ```
   p1 = *(info+0x24)   → what?
   p2 = *(p1+0x??)     → what?
   jmpbuf = *p2         → what?
   ```
4. Compare against our setup to find the mismatch

**Fix:** Adjust chain node offsets to match actual `_RTExit` code.

**Alternative:** If `_RTExit` reads `__target_for_exit` directly rather than via info+0x24, the chain may not be needed.

---

## Likely Issue 4: Unicorn `sc` PC Handling

**Symptom:** After first `sc` stub returns, PPC execution goes to 0xC00 (exception vector) instead of resuming after `sc`.

**Diagnosis:** Check `--trace-cpu` output — after the sc handler, does the next instruction address make sense?

**Fix:** Already handled in Phase 1 (set PC = SRR0 in interrupt hook). If SRR0 isn't correct, compute return address manually from the stub code layout.

---

## Likely Issue 5: CallUniversalProc Trampoline Bugs

**Symptom:** StdCLib crashes during init when calling device handlers. `--trace-toolbox` shows CallUniversalProc entered but target never reached.

**Diagnosis:**
1. Dump trampoline code from emulated memory and verify instruction encoding
2. Check `beq` branch offset (common off-by-4 error)
3. Verify arg shifting: r5→r3, r6→r4, etc.
4. Check 68K sc stub address patching

**Fix:** Correct branch offsets and verify against `DumpPEF` output.

---

## Debugging Methodology

### For any PPC crash:
1. Read PC — cross-reference with StdCLib code offsets (below)
2. Read LR and stack (r1+8) for call chain
3. Check r1 (stack), r2 (TOC), r3 (arg/return), r11 (stub index)
4. Use `memoryReadLong()` to inspect relevant structures

### StdCLib code offsets (from PPC_STDIO_FINDINGS.md):
```
__initialize:  0xF41C    Shared init:   0xEBEC
_RTInit:       0xEE98    _RTExit:       0xF050
exit:          0xD18C    fprintf:       0x68F8
_doprnt:       0x6AC0    _flsbuf:       0xDF24
_xflsbuf:      0xDB08    _coWrite:      0xCC7C
_coIoctl:      0xCD94    write:         0x10AB4
setjmp:        0xF6F8    longjmp:       0xF76C
```

Absolute address = `stdclibResult.sections[0].address + offset`.

Use `mpw DumpPEF -do All -pi u -a -fmt on ~/mpw/SharedLibraries/StdCLib` for full disassembly to correlate trace output with source logic.

---

## Validation

This phase is complete when:
1. `./bin/mpw --ppc ~/path/to/Hello` prints "Hello, world!" to stdout
2. Exit status is 0
3. No crashes or hangs
4. No stdlib function interception (exit, fflush, fprintf, etc.)

Additional validation:
- Run `HelloFlush` (explicit fflush) — should also work
- `return 1` from main — verify exit status propagates
- Longer output — verify buffering and flushing work end-to-end
