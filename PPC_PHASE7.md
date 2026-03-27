# Phase 7: Iteration and Troubleshooting

**Goal:** Diagnose and fix issues that emerge during Phase 6 integration, using the tools we've built.

**Depends on:** Phase 6 (ECON device handlers).

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

---

## Resolved Issues

### FP Unavailable Exception (RESOLVED)

PPC 603e requires MSR[FP]=1 for floating-point instructions. Without it, `lfd`/`stfd` in longjmp triggered an exception that our interrupt hook misinterpreted as an `sc` call, skipping the rest of longjmp and leaving r3=0 instead of 1.

**Fix:** Set `MSR = 0x2000` during `PPC::Init()`. Committed in `e04aafc5`.

### Exit Path (RESOLVED)

Confirmed via debugger: `exit()` → `_RTExit` → `_DoExitProcs` → dispose routine descriptors → walk exit chain (`info+0x24` → `_IntEnv+0x16` → `__target_for_exit`) → `longjmp(__target_for_exit, 1)` → `__start` cleanup → return. All working correctly.

### Exit Chain Setup (RESOLVED — not needed)

StdCLib's `shared_init_helper` writes `&_IntEnv` into `info+0x24`. `_RTInit` writes `&__target_for_exit` into the cell at `*(_IntEnv+0x16)`. We do NOT need to set up the exit chain externally. See `PPC_STDCLIB_INIT.md`.

---

## Likely Phase 6 Issues

### stdout Not Line-Buffered

**Symptom:** Tool exits cleanly but no output.

**Diagnosis:**
1. Check `--trace-mpw` for ECON ioctl calls during StdCLib init
2. Verify FIOINTERACTIVE returns 0 for stdout
3. Use debugger to inspect stdout FILE flags after init:
   ```
   ] p *($stdout_addr + 0x12)
   ```
4. Use `pef_inspect` to find stdout address: `sym stdout`

**Possible fixes:**
- Cookie `connected` flag (byte +0x0C) must be 1
- Cookie `mode` byte (+0x00) must match direction (0x02 for stdout)

### CallUniversalProc Trampoline Bugs

**Symptom:** StdCLib init hangs or crashes when calling ECON device handlers via CallUniversalProc.

**Diagnosis:**
1. Use `--debug` to break at CallUniversalProc entry
2. Check if the PPC TVector path or 68K descriptor path is taken
3. Verify arg shifting: r5→r3, r6→r4, etc.

### Missing Stubs

**Symptom:** `PPC FATAL: unimplemented stub` message.

**Fix:** Add the stub to `ppc_dispatch.cpp`.

---

## Validation

Phase 7 is complete when:
1. `./bin/mpw --ppc tools/Hello` prints "Hello, world!" and exits 0
2. No crashes, hangs, or infinite loops
3. `--trace-mpw` shows ECON write with the output data
4. 68K tools still work
