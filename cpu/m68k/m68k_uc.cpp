/*
 * m68k_uc.cpp — M68K CPU emulation via Unicorn Engine
 *
 * Drop-in replacement for the WinFellow CPU core. Implements the same
 * public API (CpuModule.h, fmem.h) using Unicorn for instruction execution
 * and Capstone for disassembly.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unicorn/unicorn.h>
#include <capstone.h>

#include <cpu/m68k/defs.h>
#include <cpu/m68k/CpuModule.h>
#include <cpu/m68k/fmem.h>

// ================================================================
// State
// ================================================================

static uc_engine *uc = nullptr;
static csh cs = 0;

static BOOLE stopped = FALSE;
static uint32_t cpuModelMajor = 3; // default 68030
static uint32_t cpuModelMinor = 0;
static uint32_t initialPC = 0;
static uint32_t initialSP = 0;
static uint32_t lastInstructionTime = 1;

// Exception callbacks
static cpuLineExceptionFunc alineFunc = nullptr;
static cpuLineExceptionFunc flineFunc = nullptr;
static cpuMidInstructionExceptionFunc midInstrFunc = nullptr;
static cpuResetExceptionFunc resetFunc = nullptr;
#ifdef CPU_INSTRUCTION_LOGGING
static cpuInstructionLoggingFunc instrLogFunc = nullptr;
static cpuExceptionLoggingFunc exceptLogFunc = nullptr;
static cpuInterruptLoggingFunc interruptLogFunc = nullptr;
#endif

// Memory
static uint8_t *Memory = nullptr;
static uint32_t MemorySize = 0;
static memoryLoggingFunc MemoryLoggingFunc = nullptr;

// Exception handler stub addresses in emulated memory
static const uint32_t kALineHandlerAddr  = 0x00000400;
static const uint32_t kFLineHandlerAddr  = 0x00000410;

// ================================================================
// Helpers
// ================================================================

static void checkErr(uc_err err, const char *context) {
	if (err != UC_ERR_OK) {
		fprintf(stderr, "M68K Unicorn error in %s: %s\n",
		        context, uc_strerror(err));
	}
}

// ================================================================
// Exception Hook
// ================================================================

// Dispatch an A-line or F-line trap from a CPU exception frame.
// The CPU has vectored to handlerAddr, pushing [SR, PC, ...] onto A7.
// This unwinds the frame, calls the trap handler, and restores state.
// Returns true if a handler was called.
static bool dispatchTrapFromExceptionFrame(uint32_t handlerAddr) {
	uint32_t sp = 0;
	uc_reg_read(uc, UC_M68K_REG_A7, &sp);

	// Stack frame: [SR(16-bit), PC(32-bit), ...]
	// For 68020+ format 0: [SR(16), PC(32), format/vector(16)]
	uint32_t stackedPC = (Memory[sp + 2] << 24) | (Memory[sp + 3] << 16) |
	                     (Memory[sp + 4] << 8) | Memory[sp + 5];

	// The trap opcode is at stackedPC - 2 (PC points after the A-line/F-line word)
	uint16_t opcode = (Memory[stackedPC - 2] << 8) | Memory[stackedPC - 1];

	cpuLineExceptionFunc handler = nullptr;
	if (handlerAddr == kALineHandlerAddr && alineFunc) {
		handler = alineFunc;
	} else if (handlerAddr == kFLineHandlerAddr && flineFunc) {
		handler = flineFunc;
	}

	if (!handler) return false;

	// Set PC to stackedPC (after the trap instruction) so the callback
	// sees the same state as with the WinFellow core.
	uc_reg_write(uc, UC_M68K_REG_PC, &stackedPC);

	handler(opcode);

	// Read potentially-modified PC (callback may have changed it)
	uint32_t newPC = 0;
	uc_reg_read(uc, UC_M68K_REG_PC, &newPC);

	// Pop the exception frame: restore SR, set PC, adjust SP.
	// For 68000/010 (format 0): frame = 6 bytes (SR + PC)
	// For 68020+ (format 2): frame = 8 bytes (SR + PC + format/vector)
	uint16_t sr = (Memory[sp] << 8) | Memory[sp + 1];
	uint32_t frameSize;
	if (cpuModelMajor >= 2) {
		// 68020+: check format word
		uint16_t formatWord = (Memory[sp + 6] << 8) | Memory[sp + 7];
		uint16_t format = (formatWord >> 12) & 0xF;
		// Format 0 = 4 words (8 bytes), Format 2 = 6 words (12 bytes)
		if (format == 0) frameSize = 8;
		else if (format == 2) frameSize = 12;
		else frameSize = 8; // fallback
	} else {
		frameSize = 6;
	}

	sp += frameSize;
	uc_reg_write(uc, UC_M68K_REG_A7, &sp);
	uint32_t srVal = sr;
	uc_reg_write(uc, UC_M68K_REG_SR, &srVal);
	uc_reg_write(uc, UC_M68K_REG_PC, &newPC);
	return true;
}

// Hook for A-line and F-line exception handler stubs (used in single-step mode).
static void exceptionHook(uc_engine *uc_, uint64_t address,
                          uint32_t size, void *user_data) {
	(void)uc_;
	(void)user_data;
	(void)size;
	dispatchTrapFromExceptionFrame((uint32_t)address);
}

// ================================================================
// CPU Module API
// ================================================================

void cpuStartup(void) {
	if (uc) {
		uc_close(uc);
		uc = nullptr;
	}

	uc_err err = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &uc);
	checkErr(err, "uc_open");
	if (err != UC_ERR_OK) return;

	// Set CPU model
	uc_cpu_m68k model;
	switch (cpuModelMajor) {
	case 0: model = UC_CPU_M68K_M68000; break;
	case 1: model = UC_CPU_M68K_M68000; break; // 68010 not separate in Unicorn
	case 2: model = UC_CPU_M68K_M68020; break;
	case 3: model = UC_CPU_M68K_M68030; break;
	default: model = UC_CPU_M68K_M68030; break;
	}
	err = uc_ctl_set_cpu_model(uc, model);
	checkErr(err, "uc_ctl_set_cpu_model");

	// Map memory if already set
	if (Memory && MemorySize > 0) {
		err = uc_mem_map_ptr(uc, 0, MemorySize, UC_PROT_ALL, Memory);
		checkErr(err, "uc_mem_map_ptr");
	}

	// Set up exception vectors for A-line and F-line
	if (Memory && MemorySize > kFLineHandlerAddr + 16) {
		// A-line vector (exception 10, offset 0x28)
		Memory[0x28] = (kALineHandlerAddr >> 24) & 0xFF;
		Memory[0x29] = (kALineHandlerAddr >> 16) & 0xFF;
		Memory[0x2A] = (kALineHandlerAddr >> 8) & 0xFF;
		Memory[0x2B] = kALineHandlerAddr & 0xFF;

		// F-line vector (exception 11, offset 0x2C)
		Memory[0x2C] = (kFLineHandlerAddr >> 24) & 0xFF;
		Memory[0x2D] = (kFLineHandlerAddr >> 16) & 0xFF;
		Memory[0x2E] = (kFLineHandlerAddr >> 8) & 0xFF;
		Memory[0x2F] = kFLineHandlerAddr & 0xFF;

		// Write NOP instructions at handler addresses (the hook will intercept)
		// 0x4E71 = NOP
		Memory[kALineHandlerAddr] = 0x4E;
		Memory[kALineHandlerAddr + 1] = 0x71;
		Memory[kFLineHandlerAddr] = 0x4E;
		Memory[kFLineHandlerAddr + 1] = 0x71;

		// Register code hooks on the handler addresses
		uc_hook hook1, hook2;
		uc_hook_add(uc, &hook1, UC_HOOK_CODE, (void *)exceptionHook,
		            nullptr, kALineHandlerAddr, kALineHandlerAddr + 1);
		uc_hook_add(uc, &hook2, UC_HOOK_CODE, (void *)exceptionHook,
		            nullptr, kFLineHandlerAddr, kFLineHandlerAddr + 1);
	}

	// Start in supervisor mode (same as WinFellow core)
	{
		uint32_t sr = 0x2000; // S bit set
		uc_reg_write(uc, UC_M68K_REG_SR, &sr);
	}

	// Initialize Capstone
	if (!cs) {
		if (cs_open(CS_ARCH_M68K, CS_MODE_M68K_030, &cs) != CS_ERR_OK) {
			fprintf(stderr, "M68K: failed to init Capstone\n");
		}
	}

	stopped = FALSE;
}

void cpuHardReset(void) {
	if (uc) cpuStartup();
}

void cpuSetModel(uint32_t major, uint32_t minor) {
	cpuModelMajor = major;
	cpuModelMinor = minor;

	if (uc) {
		uc_cpu_m68k model;
		switch (major) {
		case 0: model = UC_CPU_M68K_M68000; break;
		case 1: model = UC_CPU_M68K_M68000; break;
		case 2: model = UC_CPU_M68K_M68020; break;
		case 3: model = UC_CPU_M68K_M68030; break;
		default: model = UC_CPU_M68K_M68030; break;
		}
		uc_ctl_set_cpu_model(uc, model);
	}
}

uint32_t cpuGetModelMajor(void) { return cpuModelMajor; }
uint32_t cpuGetModelMinor(void) { return cpuModelMinor; }

// ================================================================
// Register Access
// ================================================================

void cpuSetPC(uint32_t pc) {
	if (uc) uc_reg_write(uc, UC_M68K_REG_PC, &pc);
}

uint32_t cpuGetPC(void) {
	uint32_t pc = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_PC, &pc);
	return pc;
}

void cpuSetDReg(uint32_t i, uint32_t value) {
	if (uc && i < 8) uc_reg_write(uc, UC_M68K_REG_D0 + i, &value);
}

uint32_t cpuGetDReg(uint32_t i) {
	uint32_t val = 0;
	if (uc && i < 8) uc_reg_read(uc, UC_M68K_REG_D0 + i, &val);
	return val;
}

void cpuSetAReg(uint32_t i, uint32_t value) {
	if (uc && i < 8) uc_reg_write(uc, UC_M68K_REG_A0 + i, &value);
}

uint32_t cpuGetAReg(uint32_t i) {
	uint32_t val = 0;
	if (uc && i < 8) uc_reg_read(uc, UC_M68K_REG_A0 + i, &val);
	return val;
}

void cpuSetReg(uint32_t da, uint32_t i, uint32_t value) {
	if (da) cpuSetAReg(i, value);
	else cpuSetDReg(i, value);
}

uint32_t cpuGetReg(uint32_t da, uint32_t i) {
	return da ? cpuGetAReg(i) : cpuGetDReg(i);
}

void cpuSetSR(uint32_t sr) {
	if (uc) uc_reg_write(uc, UC_M68K_REG_SR, &sr);
}

uint32_t cpuGetSR(void) {
	uint32_t sr = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_SR, &sr);
	return sr;
}

void cpuSetUspDirect(uint32_t usp) {
	if (uc) uc_reg_write(uc, UC_M68K_REG_CR_USP, &usp);
}

uint32_t cpuGetUspDirect(void) {
	uint32_t usp = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_CR_USP, &usp);
	return usp;
}

uint32_t cpuGetUspAutoMap(void) {
	// If in user mode, return A7; otherwise return USP
	uint32_t sr = cpuGetSR();
	if (!(sr & 0x2000)) return cpuGetAReg(7);
	return cpuGetUspDirect();
}

void cpuSetMspDirect(uint32_t msp) {
	if (uc) uc_reg_write(uc, UC_M68K_REG_CR_MSP, &msp);
}

uint32_t cpuGetMspDirect(void) {
	uint32_t msp = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_CR_MSP, &msp);
	return msp;
}

void cpuSetSspDirect(uint32_t ssp) {
	if (uc) uc_reg_write(uc, UC_M68K_REG_CR_ISP, &ssp);
}

uint32_t cpuGetSspDirect(void) {
	uint32_t ssp = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_CR_ISP, &ssp);
	return ssp;
}

uint32_t cpuGetSspAutoMap(void) {
	uint32_t sr = cpuGetSR();
	if (sr & 0x2000) return cpuGetAReg(7);
	return cpuGetSspDirect();
}

uint32_t cpuGetVbr(void) {
	uint32_t vbr = 0;
	if (uc) uc_reg_read(uc, UC_M68K_REG_CR_VBR, &vbr);
	return vbr;
}

void cpuSetStop(BOOLE stop) {
	stopped = stop;
	if (stop && uc) uc_emu_stop(uc);
}

BOOLE cpuGetStop(void) { return stopped; }

void cpuSetInitialPC(uint32_t pc) { initialPC = pc; }
uint32_t cpuGetInitialPC(void) { return initialPC; }
void cpuSetInitialSP(uint32_t sp) { initialSP = sp; }
uint32_t cpuGetInitialSP(void) { return initialSP; }

uint32_t cpuGetInstructionTime(void) { return lastInstructionTime; }

BOOLE cpuSetIrqLevel(uint32_t irq_level) {
	(void)irq_level;
	return FALSE;
}

uint32_t cpuGetIrqLevel(void) { return 0; }

// ================================================================
// Execution
// ================================================================

uint32_t cpuExecuteInstruction(void) {
	if (!uc || stopped) return 0;

#ifdef CPU_INSTRUCTION_LOGGING
	if (instrLogFunc) instrLogFunc();
#endif

	uint32_t pc = cpuGetPC();
	uc_err err = uc_emu_start(uc, pc, 0, 0, 1);

	if (err == UC_ERR_EXCEPTION) {
		// Unicorn raised a CPU exception (likely A-line or F-line trap).
		// The PC still points at the trapping instruction. Read the opcode
		// and dispatch to the appropriate handler.
		uint32_t excPC = cpuGetPC();
		if (excPC + 1 < MemorySize) {
			uint16_t opcode = (Memory[excPC] << 8) | Memory[excPC + 1];

			if ((opcode & 0xF000) == 0xA000 && alineFunc) {
				// A-line trap: advance PC past the 2-byte instruction,
				// then call the handler (same behavior as WinFellow).
				cpuSetPC(excPC + 2);
				alineFunc(opcode);
			} else if ((opcode & 0xF000) == 0xF000 && flineFunc) {
				// F-line trap
				cpuSetPC(excPC + 2);
				flineFunc(opcode);
			} else {
				fprintf(stderr, "M68K: unhandled exception at PC=%08X opcode=%04X\n",
				        excPC, opcode);
			}
		}
	} else if (err != UC_ERR_OK && err != UC_ERR_FETCH_UNMAPPED) {
		if (!stopped) {
			fprintf(stderr, "M68K: execution error at PC=%08X: %s\n",
			        cpuGetPC(), uc_strerror(err));
		}
	}

	lastInstructionTime = 1;
	return lastInstructionTime;
}

// ================================================================
// Batch Execution (Unicorn exits)
// ================================================================

void cpuEnableBatchMode(void) {
	if (!uc) return;
	uc_ctl_exits_enable(uc);
	uint64_t exits[] = { kALineHandlerAddr, kFLineHandlerAddr };
	uc_ctl_set_exits(uc, exits, 2);
}

void cpuDisableBatchMode(void) {
	if (!uc) return;
	uc_ctl_exits_disable(uc);
}

bool cpuRunBatch(void) {
	if (!uc || stopped) return false;

	uint32_t pc = cpuGetPC();
	uc_err err = uc_emu_start(uc, pc, 0, 0, 0);

	if (stopped) return false;

	uint32_t newPC = cpuGetPC();

	if (err == UC_ERR_OK) {
		// Stopped at an exit address (A-line or F-line handler)
		if (newPC == kALineHandlerAddr || newPC == kFLineHandlerAddr) {
			dispatchTrapFromExceptionFrame(newPC);
			return !stopped;
		}
		// uc_emu_stop was called (e.g. from a trap handler)
		return false;
	}

	if (err == UC_ERR_EXCEPTION) {
		// Fallback: CPU didn't vector, PC is at the trapping instruction
		if (newPC + 1 < MemorySize) {
			uint16_t opcode = (Memory[newPC] << 8) | Memory[newPC + 1];
			if ((opcode & 0xF000) == 0xA000 && alineFunc) {
				cpuSetPC(newPC + 2);
				alineFunc(opcode);
			} else if ((opcode & 0xF000) == 0xF000 && flineFunc) {
				cpuSetPC(newPC + 2);
				flineFunc(opcode);
			} else {
				fprintf(stderr, "M68K: unhandled exception at PC=%08X opcode=%04X\n",
				        newPC, opcode);
			}
		}
		return !stopped;
	}

	if (err == UC_ERR_FETCH_UNMAPPED) {
		// PC went to unmapped memory (e.g. PC=0 means program exit)
		return false;
	}

	fprintf(stderr, "M68K: batch execution error at PC=%08X: %s\n",
	        newPC, uc_strerror(err));
	return false;
}

// ================================================================
// Disassembly
// ================================================================

uint32_t cpuDisOpcode(uint32_t disasm_pc, char *saddress, char *sdata,
                      char *sinstruction, char *soperands) {
	if (!Memory || disasm_pc >= MemorySize) {
		sprintf(saddress, "$%08X", disasm_pc);
		strcpy(sdata, "????");
		strcpy(sinstruction, "???");
		soperands[0] = 0;
		return disasm_pc + 2;
	}

	cs_insn *insn;
	size_t n = cs_disasm(cs, Memory + disasm_pc, MemorySize - disasm_pc,
	                     disasm_pc, 1, &insn);
	if (n > 0) {
		sprintf(saddress, "$%08X", (uint32_t)insn[0].address);

		// Format raw bytes
		sdata[0] = 0;
		for (int i = 0; i < insn[0].size && i < 10; i += 2) {
			char hex[8];
			if (i + 1 < insn[0].size)
				sprintf(hex, "%02X%02X", insn[0].bytes[i], insn[0].bytes[i+1]);
			else
				sprintf(hex, "%02X", insn[0].bytes[i]);
			strcat(sdata, hex);
		}

		snprintf(sinstruction, 64, "%-10s", insn[0].mnemonic);
		snprintf(soperands, 256, "%s", insn[0].op_str);

		uint32_t nextPC = (uint32_t)insn[0].address + insn[0].size;
		cs_free(insn, n);
		return nextPC;
	}

	// Fallback
	sprintf(saddress, "$%08X", disasm_pc);
	sprintf(sdata, "%04X",
	        (Memory[disasm_pc] << 8) | Memory[disasm_pc + 1]);
	strcpy(sinstruction, ".word");
	sprintf(soperands, "$%04X",
	        (Memory[disasm_pc] << 8) | Memory[disasm_pc + 1]);
	return disasm_pc + 2;
}

// ================================================================
// Callback Registration
// ================================================================

void cpuSetALineExceptionFunc(cpuLineExceptionFunc func) { alineFunc = func; }
void cpuSetFLineExceptionFunc(cpuLineExceptionFunc func) { flineFunc = func; }
void cpuSetMidInstructionExceptionFunc(cpuMidInstructionExceptionFunc func) { midInstrFunc = func; }
void cpuSetResetExceptionFunc(cpuResetExceptionFunc func) { resetFunc = func; }
void cpuThrowAddressErrorException(void) { if (midInstrFunc) midInstrFunc(); }

void cpuSetCheckPendingInterruptsFunc(cpuCheckPendingInterruptsFunc func) { (void)func; }
void cpuCheckPendingInterrupts(void) {}
void cpuSetUpInterrupt(uint32_t new_interrupt_level) { (void)new_interrupt_level; }
void cpuInitializeFromNewPC(uint32_t new_pc) { cpuSetPC(new_pc); }

#ifdef CPU_INSTRUCTION_LOGGING
void cpuSetInstructionLoggingFunc(cpuInstructionLoggingFunc func) { instrLogFunc = func; }
void cpuSetExceptionLoggingFunc(cpuExceptionLoggingFunc func) { exceptLogFunc = func; }
void cpuSetInterruptLoggingFunc(cpuInterruptLoggingFunc func) { interruptLogFunc = func; }
#endif

void cpuSaveState(FILE *F) { (void)F; }
void cpuLoadState(FILE *F) { (void)F; }

// Not in CpuModule.h but declared extern in loader.cpp
extern "C" void cpuSetRaiseInterrupt(BOOLE raise_irq) { (void)raise_irq; }

// Internal flag-setting functions used by sane.cpp and dispatch.cpp.
// These modify the condition code register (lower 8 bits of SR).
// CCR bits: X=4, N=3, Z=2, V=1, C=0

extern "C" void cpuSetFlagsNZ00NewW(uint16_t res) {
	uint32_t sr = cpuGetSR();
	sr &= 0xFFF0; // clear N, Z, V, C (keep X)
	if (res == 0) sr |= 0x04; // Z
	if (res & 0x8000) sr |= 0x08; // N
	cpuSetSR(sr);
}

extern "C" void cpuSetFlagsShift(BOOLE z, BOOLE n, BOOLE c, BOOLE v) {
	uint32_t sr = cpuGetSR();
	sr &= 0xFFF0;
	if (z) sr |= 0x04;
	if (n) sr |= 0x08;
	if (v) sr |= 0x02;
	if (c) sr |= 0x01;
	cpuSetSR(sr);
}

extern "C" void cpuSetFlagsAbs(uint16_t f) {
	uint32_t sr = cpuGetSR();
	sr = (sr & 0xFF00) | (f & 0xFF);
	cpuSetSR(sr);
}

// ================================================================
// Memory
// ================================================================

void memorySetMemory(uint8_t *memory, uint32_t size) {
	Memory = memory;
	MemorySize = size;

	if (uc && memory && size > 0) {
		uc_err err = uc_mem_map_ptr(uc, 0, size, UC_PROT_ALL, memory);
		checkErr(err, "memorySetMemory/uc_mem_map_ptr");
	}
}

void memorySetGlobalLog(uint32_t globalLog) { (void)globalLog; }

uint8_t *memoryPointer(uint32_t address) {
	return Memory + address;
}

uint8_t memoryReadByte(uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 1, 0, 0);
	if (address < MemorySize) return Memory[address];
	return 0;
}

uint16_t memoryReadWord(uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 2, 0, 0);
	if (address + 1 < MemorySize)
		return (Memory[address] << 8) | Memory[address + 1];
	return 0;
}

uint32_t memoryReadLong(uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 4, 0, 0);
	if (address + 3 < MemorySize)
		return ((uint32_t)Memory[address] << 24) |
		       ((uint32_t)Memory[address + 1] << 16) |
		       ((uint32_t)Memory[address + 2] << 8) |
		       Memory[address + 3];
	return 0;
}

uint64_t memoryReadLongLong(uint32_t address) {
	return ((uint64_t)memoryReadLong(address) << 32) | memoryReadLong(address + 4);
}

void memoryWriteByte(uint8_t data, uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 1, 1, data);
	if (address < MemorySize) Memory[address] = data;
}

void memoryWriteWord(uint16_t data, uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 2, 1, data);
	if (address + 1 < MemorySize) {
		Memory[address] = (data >> 8) & 0xFF;
		Memory[address + 1] = data & 0xFF;
	}
}

void memoryWriteLong(uint32_t data, uint32_t address) {
	if (MemoryLoggingFunc) MemoryLoggingFunc(address, 4, 1, data);
	if (address + 3 < MemorySize) {
		Memory[address] = (data >> 24) & 0xFF;
		Memory[address + 1] = (data >> 16) & 0xFF;
		Memory[address + 2] = (data >> 8) & 0xFF;
		Memory[address + 3] = data & 0xFF;
	}
}

void memoryWriteLongLong(uint64_t data, uint32_t address) {
	memoryWriteLong((uint32_t)(data >> 32), address);
	memoryWriteLong((uint32_t)(data & 0xFFFFFFFF), address + 4);
}

void memoryWriteLongToPointer(uint32_t data, uint8_t *address) {
	address[0] = (data >> 24) & 0xFF;
	address[1] = (data >> 16) & 0xFF;
	address[2] = (data >> 8) & 0xFF;
	address[3] = data & 0xFF;
}

void memorySetLoggingFunc(memoryLoggingFunc func) { MemoryLoggingFunc = func; }

// Chip memory stubs (not used by MPW)
uint16_t memoryChipReadWord(uint32_t address) { return memoryReadWord(address); }
void memoryChipWriteWord(uint16_t data, uint32_t address) { memoryWriteWord(data, address); }
