# Phase 1: PPC CPU Core (Unicorn Engine)

**Goal:** Integrate Unicorn Engine as the PowerPC execution engine, wrapped in a clean C++ interface.

**Depends on:** Nothing (first phase).

---

## Overview

Unicorn Engine wraps QEMU's CPU emulation in a C API. We use it for PPC 32-bit big-endian emulation. Key Unicorn features we rely on:

- `uc_mem_map_ptr()` — maps our existing emulated memory buffer directly into Unicorn's address space (zero-copy sharing with the 68K emulator)
- `UC_HOOK_INTR` — fires on the `sc` (system call) instruction, which is how we intercept CFM stub calls

---

## Files to Create

### `cpu/ppc/ppc.h`

The public interface for PPC emulation. All Unicorn details are hidden behind this API.

```cpp
#ifndef __cpu_ppc_h__
#define __cpu_ppc_h__

#include <cstdint>
#include <functional>

namespace PPC {
    // Called when PPC executes an `sc` instruction.
    // The handler should read/write PPC registers as needed.
    using SCHandler = std::function<void()>;

    // Initialize the PPC engine. Maps `memory[0..memorySize)` as the
    // emulated address space. Must be called before any other PPC function.
    // `memory` must be page-aligned (4096 bytes) and `memorySize` must be
    // a multiple of 4096.
    void Init(uint8_t *memory, uint32_t memorySize);

    // Shut down the PPC engine and release resources.
    void Shutdown();

    // Execute PPC code starting at `pc` with TOC register r2 set to `toc`.
    // Runs until Stop() is called or PC becomes 0 (sentinel for "return
    // from top-level call").
    void Execute(uint32_t pc, uint32_t toc);

    // Stop execution. Safe to call from within an SCHandler callback.
    void Stop();

    // Register access — GPR (r0-r31)
    uint32_t GetGPR(int reg);
    void     SetGPR(int reg, uint32_t value);

    // Register access — FPR (f0-f31)
    double GetFPR(int reg);
    void   SetFPR(int reg, double value);

    // Program counter
    uint32_t GetPC();
    void     SetPC(uint32_t pc);

    // Link register
    uint32_t GetLR();
    void     SetLR(uint32_t lr);

    // Count register
    uint32_t GetCTR();
    void     SetCTR(uint32_t ctr);

    // Condition register
    uint32_t GetCR();
    void     SetCR(uint32_t cr);

    // XER (fixed-point exception register)
    uint32_t GetXER();
    void     SetXER(uint32_t xer);

    // Set the handler for `sc` instructions.
    void SetSCHandler(SCHandler handler);

    // Tracing: enable per-instruction trace output to stderr.
    // Activated by the existing --trace-cpu flag when running a PPC tool.
    void SetTraceCode(bool enable);
}

#endif
```

### `cpu/ppc/ppc.cpp`

The Unicorn wrapper implementation. Key implementation details:

**Initialization:**
```cpp
#include <unicorn/unicorn.h>

static uc_engine *uc = nullptr;
static PPC::SCHandler scHandler;
static bool stopped = false;

void PPC::Init(uint8_t *memory, uint32_t memorySize) {
    uc_err err = uc_open(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN, &uc);
    // check err

    // Map the existing memory buffer into Unicorn.
    // This shares the same physical memory with the 68K emulator.
    err = uc_mem_map_ptr(uc, 0, memorySize,
                         UC_PROT_ALL, memory);
    // check err

    // Register the interrupt hook to catch `sc` instructions.
    uc_hook hk;
    err = uc_hook_add(uc, &hk, UC_HOOK_INTR, (void *)interruptHook, nullptr, 1, 0);
    // check err
}
```

**Interrupt hook (sc handler):**

When PPC executes `sc`, the CPU raises a System Call Exception. Unicorn fires our `UC_HOOK_INTR` callback. The PPC has already set SRR0 to the address of the instruction following `sc`. We need to:
1. Dispatch to the registered sc handler
2. Set PC to SRR0 (the return address) so execution resumes after the `sc`

```cpp
static void interruptHook(uc_engine *uc, uint32_t intno, void *user_data) {
    // intno 8 = PPC system call exception (vector 0xC00)
    // Read SRR0 — the return address after sc
    uint32_t srr0;
    uc_reg_read(uc, UC_PPC_REG_SRR0, &srr0);

    // Dispatch to the registered handler
    if (scHandler) {
        scHandler();
    }

    // If Stop() was called during the handler, don't resume
    if (stopped) {
        uc_emu_stop(uc);
        return;
    }

    // Set PC to the instruction after sc
    uc_reg_write(uc, UC_PPC_REG_PC, &srr0);
}
```

**Execute function:**

```cpp
void PPC::Execute(uint32_t pc, uint32_t toc) {
    stopped = false;
    SetGPR(2, toc);
    SetPC(pc);

    // Run until:
    // - Stop() is called (from sc handler or elsewhere)
    // - An error occurs
    // We use uc_emu_start with count=0 (unlimited) and timeout=0 (unlimited)
    // and rely on Stop()/uc_emu_stop() to halt execution.
    //
    // Note: We need to handle the "PC becomes 0" sentinel. We can do this
    // with a code hook at address 0, or by checking PC after each sc return.
    // Simplest: add a UC_HOOK_CODE hook at address 0 that calls uc_emu_stop.

    uc_err err = uc_emu_start(uc, pc, 0 /* unused */, 0, 0);
    // UC_ERR_OK or UC_ERR_FETCH_UNMAPPED (if PC=0) are expected
}
```

**Stop function:**
```cpp
void PPC::Stop() {
    stopped = true;
    uc_emu_stop(uc);
}
```

**Instruction tracing (SetTraceCode):**

Use Unicorn's `UC_HOOK_CODE` to log every executed instruction. This is the PPC equivalent of the existing `--trace-cpu` flag for 68K. The hook fires before each instruction:

```cpp
static bool traceCodeEnabled = false;
static uc_hook traceCodeHook = 0;

static void codeTraceCallback(uc_engine *uc, uint64_t address,
                               uint32_t size, void *user_data) {
    uint32_t instr = 0;
    uc_mem_read(uc, address, &instr, 4);
    instr = __builtin_bswap32(instr);  // Unicorn reads in host byte order

    // Print address, raw instruction, and register context
    uint32_t lr, r1, r3;
    uc_reg_read(uc, UC_PPC_REG_LR, &lr);
    uc_reg_read(uc, UC_PPC_REG_1, &r1);
    uc_reg_read(uc, UC_PPC_REG_3, &r3);
    fprintf(stderr, "  PPC %08X: %08X  (LR=%08X SP=%08X R3=%08X)\n",
            (uint32_t)address, instr, lr, r1, r3);
}

void PPC::SetTraceCode(bool enable) {
    traceCodeEnabled = enable;
    if (enable && !traceCodeHook) {
        uc_hook_add(uc, &traceCodeHook, UC_HOOK_CODE,
                    (void *)codeTraceCallback, nullptr, 1, 0);
    }
    // Note: Unicorn doesn't support removing hooks easily; leave installed
    // but check traceCodeEnabled in the callback if perf is a concern.
}
```

This is critical for debugging — when something goes wrong in StdCLib's PPC code, the instruction trace reveals exactly where execution diverged from expectations. The trace output can be cross-referenced with `mpw DumpPEF -do All -pi u -a -fmt on` disassembly to understand what StdCLib is doing.

The existing `--trace-cpu` flag is reused for PPC: when running a PPC tool, it enables this PPC instruction trace instead of the 68K one.

**Register access (pattern for all registers):**
```cpp
uint32_t PPC::GetGPR(int reg) {
    uint32_t val;
    uc_reg_read(uc, UC_PPC_REG_0 + reg, &val);
    return val;
}

void PPC::SetGPR(int reg, uint32_t value) {
    uc_reg_write(uc, UC_PPC_REG_0 + reg, &value);
}

double PPC::GetFPR(int reg) {
    double val;
    uc_reg_read(uc, UC_PPC_REG_FPR0 + reg, &val);
    return val;
}
// ... etc for LR, CTR, CR, XER, PC using their UC_PPC_REG_* constants
```

**Sentinel for top-level return:**

When we call a PPC function, we set LR=0 as a sentinel. When the function returns (`blr`), PC becomes 0. We handle this by:
- Adding a `UC_HOOK_CODE` hook at address 0 that calls `uc_emu_stop()`
- OR: Unicorn will raise `UC_ERR_FETCH_UNMAPPED` when PC=0 (if address 0 is not mapped), which also stops execution

The cleaner approach: don't map address 0 (map memory starting from a non-zero base, or map 0-4095 as UC_PROT_NONE). Then `uc_emu_start` returns `UC_ERR_FETCH_UNMAPPED` when PC hits 0.

**Important consideration about memory alignment:**

`uc_mem_map_ptr()` requires the start address to be 4KB-aligned and the size to be a 4KB multiple. The existing memory allocation in `loader.cpp` uses `new uint8_t[size]` which is NOT guaranteed to be page-aligned.

We need to modify memory allocation to use page-aligned allocation:
```cpp
// In loader.cpp, change:
//   Memory = new uint8_t[Flags.memorySize];
// To:
//   Memory = (uint8_t *)aligned_alloc(4096, Flags.memorySize);
// And memorySize should be rounded up to a 4096 multiple.
```

This change is backward-compatible with the 68K path.

### `cpu/ppc/CMakeLists.txt`

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED unicorn)

add_library(PPC_LIB ppc.cpp)
target_include_directories(PPC_LIB PUBLIC ${CMAKE_SOURCE_DIR} ${UNICORN_INCLUDE_DIRS})
target_link_libraries(PPC_LIB ${UNICORN_LIBRARIES})
```

## Files to Modify

### `cpu/CMakeLists.txt`

Add `add_subdirectory(ppc)` alongside the existing `add_subdirectory(m68k)`.

### `bin/CMakeLists.txt`

Add `PPC_LIB` to the mpw target's link libraries.

### `bin/loader.cpp`

Change memory allocation to page-aligned:
```cpp
// Round up to 4096 multiple
Flags.memorySize = (Flags.memorySize + 4095) & ~4095;
Memory = Flags.memory = (uint8_t *)aligned_alloc(4096, Flags.memorySize);
memset(Memory, 0, Flags.memorySize);
```

### Top-level `CMakeLists.txt`

No changes needed — `pkg_check_modules` is called in the `cpu/ppc/CMakeLists.txt`.

---

## Implementation Steps

1. Install Unicorn: `brew install unicorn`
2. Create `cpu/ppc/` directory
3. Write `cpu/ppc/CMakeLists.txt` — find Unicorn, build PPC_LIB
4. Write `cpu/ppc/ppc.h` — the public interface (including trace APIs)
5. Write `cpu/ppc/ppc.cpp` — Unicorn wrapper:
   - `Init()` — open engine, map memory, register hooks
   - `Shutdown()` — close engine
   - `Execute()` — start emulation loop
   - `Stop()` — halt from callback
   - Interrupt hook — dispatch sc, set PC to SRR0
   - Register accessors — GetGPR/SetGPR, GetFPR/SetFPR, GetPC/SetPC, GetLR/SetLR, etc.
   - `SetTraceCode()` — per-instruction trace via UC_HOOK_CODE
6. Modify `cpu/CMakeLists.txt` — add ppc subdirectory
7. Modify `bin/CMakeLists.txt` — link PPC_LIB
8. Modify `bin/loader.cpp` — page-aligned memory allocation. The existing `--trace-cpu` flag will be wired to `PPC::SetTraceCode()` in Phase 5 when the PPC execution path is added.

---

## Validation

### Build test
```bash
cd build && cmake .. && make
```
Should compile and link cleanly. Existing 68K functionality must not be broken.

### Functional test

Add a temporary test in `loader.cpp` (or a standalone test file) that:

1. Initializes PPC with the emulated memory buffer
2. Writes a small PPC program into memory at a known address:
   ```
   li    r3, 42          # 0x38600000 | 42 = 0x3860002A
   li    r4, 100         # 0x38800064
   add   r5, r3, r4      # 0x7CA31A14 (add r5, r3, r4 — actually 0x7CA32214)
   sc                    # 0x44000002
   blr                   # 0x4E800020
   ```
3. Sets an sc handler that:
   - Reads r3, r4, r5 and verifies r3==42, r4==100, r5==142
   - Prints "sc handler fired: r3=%d r4=%d r5=%d"
4. Sets LR=0 (sentinel)
5. Calls `PPC::Execute(codeAddress, 0)`
6. Verifies execution stopped (blr to LR=0)

**Expected output:** "sc handler fired: r3=42 r4=100 r5=142"

### Verify memory sharing

After the PPC test runs, read the memory locations modified by PPC via `memoryReadLong()` (the 68K memory API) to confirm they're the same buffer.

### Trace test

Enable `PPC::SetTraceCode(true)` during the functional test. Verify that each instruction's address and raw hex is printed to stderr, matching the instructions we wrote into memory. This confirms tracing works before we need it for debugging StdCLib in later phases.

### Edge cases to verify
- `uc_emu_start` returns cleanly when PC hits 0 (unmapped fetch)
- `Stop()` called from within the sc handler actually stops execution
- Multiple `Execute()` calls work (Unicorn engine is reusable)
- Trace hook fires correctly and can be enabled/disabled

---

## Risk Notes

- **Unicorn PPC `sc` interrupt number**: The interrupt hook receives `intno`. For PPC system calls, this should be a specific value (likely 8 for the System Call Exception). Verify at runtime and filter if needed.
- **SRR0/SRR1 handling**: Unicorn may or may not set SRR0 before calling the hook. If SRR0 is not set, we need to compute the return address manually (current PC + 4). Test both approaches.
- **Memory mapping conflict at address 0**: If the existing memory starts at address 0, we can't leave it unmapped for the sentinel. Alternative: map the first page as read-only (no execute) so blr to 0 raises `UC_ERR_FETCH_PROT`.
