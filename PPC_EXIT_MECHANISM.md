# PPC Exit Mechanism: __target_for_exit

## Overview

When a PPC MPW tool calls `exit()`, control must return to the tool's `__start` function so it can read the exit status and return cleanly. This is accomplished via `setjmp`/`longjmp` through a shared buffer called `__target_for_exit`.

## Key Symbols

All imported by the Hello tool from StdCLib:

| Symbol | Type | Purpose |
|--------|------|---------|
| `__target_for_exit` | Data | A `jmp_buf` buffer in StdCLib's data section (~70 bytes). Holds saved CPU state for the setjmp/longjmp exit path. |
| `__setjmp` | TVector | Saves current CPU state (GPRs, LR, CR, SP, etc.) into a jmp_buf buffer. Returns 0 on initial call, non-zero on longjmp return. |
| `exit` | TVector | Calls `_RTExit`, which calls `longjmp(__target_for_exit, 1)`. |
| `_exit_status` | Data | A word in StdCLib's data section where `_RTExit` stores the exit code before calling longjmp. |
| `_IntEnv` | Data | Pointer to the MPGM info block. `__start` reads argc/argv/envp from here. |
| `__C_phase` | Data | CRT initialization phase counter. `__start` sets `*__C_phase = 2` before calling main. |

## Hello Tool's `__start` Flow

The tool's entry point TVector points to `__start` in the tool's own code section. After relocation, the data section's TOC layout is:

```
RTOC-0x30: TVector ptr → _BreakPoint
RTOC-0x2C: TVector ptr → __setjmp
RTOC-0x28: TVector ptr → fprintf
RTOC-0x24: ptr → _IntEnv
RTOC-0x20: ptr → __C_phase
RTOC-0x1C: ptr → __target_for_exit
RTOC-0x18: ptr → _exit_status
RTOC-0x14: TVector ptr → exit
RTOC-0x10: ptr → _iob
RTOC-0x0C: ptr → __NubAt3
RTOC+0x00: entry TVector {code_addr, toc_addr}
RTOC+0x08: "Hello, world!\r\0\0"
```

### Annotated `__start` code (from DumpPEF)

```
__start:
    mflr    r0              ; save caller's LR
    stw     r31, -4(SP)     ; save r31
    stw     r0, 8(SP)       ; save LR on stack
    stwu    SP, -64(SP)     ; allocate stack frame

    ; Check __C_phase — if non-zero, call _BreakPoint (debugger hook)
    lwz     r3, -0x0C(RTOC) ; r3 = &__C_phase
    lwz     r31, -0x24(RTOC); r31 = &_IntEnv (saved for later argc/argv access)
    lwz     r4, 0(r3)       ; r4 = *__C_phase
    cmpwi   r4, 0
    beq     skip_break      ; if __C_phase == 0, skip breakpoint

    lwz     r3, -0x04(RTOC) ; load arg for _BreakPoint
    bl      _BreakPoint_glue
    lwz     RTOC, 0x14(SP)  ; restore TOC after cross-fragment call

skip_break:
    ; Set __C_phase = 2 (indicates main is about to be called)
    lwz     r4, -0x20(RTOC) ; r4 = &__C_phase
    li      r5, 2
    stw     r5, 0(r4)       ; *__C_phase = 2

    ; *** THE KEY CALL: setjmp(__target_for_exit) ***
    lwz     r3, -0x1C(RTOC) ; r3 = &__target_for_exit (the jmp_buf)
    bl      __setjmp_glue    ; setjmp saves CPU state into __target_for_exit
    lwz     RTOC, 0x14(SP)  ; restore TOC

    ; Check setjmp return value
    cmpwi   r3, 0
    bne     cleanup          ; if non-zero, we got here via longjmp → skip to cleanup

    ; *** Normal path: setjmp returned 0 ***
    ; Read argc/argv/envp from _IntEnv (MPGM info block)
    lwz     r3, 0x02(r31)   ; r3 = argc
    lwz     r4, 0x06(r31)   ; r4 = argv
    lwz     r5, 0x0A(r31)   ; r5 = envp
    bl      main             ; call main(argc, argv, envp)
    nop                      ; (reserved for TOC restore if main were cross-fragment)
    bl      exit_glue        ; call exit(return_value_in_r3)
    lwz     RTOC, 0x14(SP)  ; restore TOC (never reached if exit works)

cleanup:
    ; *** longjmp return path: setjmp returned non-zero ***
    ; _RTExit already stored the exit code in *_exit_status
    lwz     r4, -0x18(RTOC) ; r4 = &_exit_status
    lwz     r3, 0(r4)       ; r3 = *_exit_status (the exit code)

    ; Tear down stack frame and return
    addi    SP, SP, 64
    lwz     r0, 8(SP)
    mtlr    r0
    lwz     r31, -4(SP)
    blr                      ; return to caller with r3 = exit status
```

## The Exit Sequence

### Step 1: `__start` calls `setjmp(__target_for_exit)`

`__setjmp` saves the entire CPU state into the `__target_for_exit` buffer:
- All 32 GPRs (r0-r31)
- LR (link register — the return address)
- CR (condition register)
- SP (stack pointer, r1)
- TOC (r2)

Returns 0. Execution continues to `main()`.

### Step 2: `main()` runs and returns

`main()` returns its exit code in r3. `__start` calls `exit(r3)`.

### Step 3: `exit()` → `_RTExit()`

StdCLib's `exit()` (at code offset 0xD18C) calls `_RTExit` (at code offset 0xF050).

`_RTExit`:
1. Stores the exit code in `*_exit_status`
2. Checks the StandAlone flag at `RTOC+0x1BD0`
3. If StandAlone == 0 (running under MPW shell): calls `longjmp(__target_for_exit, 1)`
4. If StandAlone != 0: calls `ExitToShell`

### Step 4: `longjmp(__target_for_exit, 1)`

Restores the CPU state saved by `setjmp`: all GPRs, LR, CR, SP. Execution resumes at the instruction after the `setjmp` call in `__start`, but now r3 = 1 (the second argument to `longjmp`).

### Step 5: `__start` cleanup

`cmpwi r3, 0` → r3 is 1, so `bne cleanup` is taken. `__start` reads `*_exit_status`, puts it in r3, and returns via `blr`.

## Open Question: info+0x24

The PPC_STDIO_FINDINGS.md describes an exit chain at MPGM `info+0x24` that `_RTExit` walks to find the jmp_buf:

```
p1 = *(info + 0x24)
p2 = *(p1 + 0x16)
jmpbuf = *p2
longjmp(jmpbuf, 1)
```

But `__target_for_exit` is in StdCLib's own data section, and `_RTExit` is also in StdCLib. So `_RTExit` may access `__target_for_exit` directly via its own TOC, without needing the `info+0x24` chain at all.

**To determine which path `_RTExit` takes:** trace its code with `--trace-cpu` starting at StdCLib code offset 0xF050, or disassemble it with `mpw DumpPEF -do All -pi u -a -fmt on ~/mpw/Libraries/SharedLibraries/StdCLib` and look for loads from `info+0x24` vs loads from RTOC-relative offsets.

If `_RTExit` accesses `__target_for_exit` directly, we do NOT need to set up `info+0x24`. The tool's `__setjmp` writes to the buffer, StdCLib's `longjmp` reads from it, and it all works through shared data section addresses.
