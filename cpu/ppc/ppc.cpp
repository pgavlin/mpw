/*
 * ppc.cpp
 *
 * PowerPC emulation wrapper around Unicorn Engine.
 * Provides a clean C++ interface hiding all Unicorn details.
 */

#include "ppc.h"

#include <unicorn/unicorn.h>
#include <unicorn/ppc.h>

#include <cstdio>
#include <cstdlib>

namespace {

	uc_engine *uc = nullptr;
	PPC::SCHandler scHandler;
	bool stopped = false;
	bool traceCode = false;
	uc_hook intrHook = 0;
	uc_hook codeHook = 0;

	void checkErr(uc_err err, const char *context) {
		if (err != UC_ERR_OK) {
			fprintf(stderr, "PPC Unicorn error in %s: %s\n",
			        context, uc_strerror(err));
		}
	}

	// Interrupt hook -- fires when PPC executes sc.
	//
	// Unicorn/QEMU raises POWERPC_EXCP_SYSCALL (intno varies).
	// At hook time, PC may be at the exception vector (0xC00) or at the
	// sc instruction itself, depending on Unicorn internals. Since the
	// Unicorn PPC header does not expose SRR0, we cannot read the saved
	// return address directly.
	//
	// Our approach: sc is only used in CFM stubs, which have the fixed
	// layout  li r11,<index>; sc; blr  (at offsets 0, 4, 8 within each
	// 12-byte stub). The caller invoked the stub via bctrl or bl, which
	// set LR to the caller's return address. After the sc handler runs,
	// we set PC = LR to return directly to the caller, skipping the blr.
	// This is correct because the stub's blr would do the same thing.
	void interruptHook(uc_engine *uc_, uint32_t intno, void *user_data) {
		(void)user_data;

		if (scHandler) {
			scHandler();
		}

		if (stopped) {
			uc_emu_stop(uc_);
			return;
		}

		// Return to the caller by setting PC = LR.
		uint32_t lr;
		uc_reg_read(uc_, UC_PPC_REG_LR, &lr);
		uc_reg_write(uc_, UC_PPC_REG_PC, &lr);
	}

	// Code trace hook -- fires before each instruction when tracing.
	void codeTraceHook_cb(uc_engine *uc_, uint64_t address,
	                      uint32_t size, void *user_data) {
		(void)user_data;
		(void)size;

		if (!traceCode) return;

		// Read the instruction (big-endian in emulated memory)
		uint32_t instr = 0;
		uc_mem_read(uc_, address, &instr, 4);
		// Unicorn reads raw bytes; PPC is big-endian, so on little-endian
		// host we need to swap for display.
		uint32_t instrBE = __builtin_bswap32(instr);

		uint32_t lr, r1, r2, r3;
		uc_reg_read(uc_, UC_PPC_REG_LR, &lr);
		uc_reg_read(uc_, UC_PPC_REG_1, &r1);
		uc_reg_read(uc_, UC_PPC_REG_2, &r2);
		uc_reg_read(uc_, UC_PPC_REG_3, &r3);
		fprintf(stderr, "  PPC %08X: %08X  LR=%08X SP=%08X TOC=%08X R3=%08X\n",
		        (uint32_t)address, instrBE, lr, r1, r2, r3);
	}

} // anonymous namespace


namespace PPC {

	void Init(uint8_t *memory, uint32_t memorySize) {
		if (uc) {
			Shutdown();
		}

		uc_err err = uc_open(UC_ARCH_PPC,
		                     (uc_mode)(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &uc);
		checkErr(err, "uc_open");
		if (err != UC_ERR_OK) return;

		// Select 603e CPU model
		err = uc_ctl_set_cpu_model(uc, UC_CPU_PPC32_603E_V4_1);
		checkErr(err, "uc_ctl_set_cpu_model");

		// Map the existing emulated memory buffer into Unicorn.
		// This shares the same physical memory with the 68K emulator.
		err = uc_mem_map_ptr(uc, 0, memorySize, UC_PROT_ALL, memory);
		checkErr(err, "uc_mem_map_ptr");
		if (err != UC_ERR_OK) return;

		// Register the interrupt hook to catch sc instructions.
		err = uc_hook_add(uc, &intrHook, UC_HOOK_INTR,
		                  (void *)interruptHook, nullptr, 1, 0);
		checkErr(err, "uc_hook_add(INTR)");

		stopped = false;
	}

	void Shutdown() {
		if (uc) {
			uc_close(uc);
			uc = nullptr;
		}
		intrHook = 0;
		codeHook = 0;
	}

	void Execute(uint32_t pc, uint32_t toc) {
		if (!uc) return;

		stopped = false;
		SetGPR(2, toc);

		// Run until stopped or error (e.g., fetch from unmapped address 0).
		uc_err err = uc_emu_start(uc, pc, 0, 0, 0);

		// UC_ERR_FETCH_UNMAPPED is expected when PC hits 0 (our sentinel
		// for "return from top-level call" via blr with LR=0).
		if (err != UC_ERR_OK && err != UC_ERR_FETCH_UNMAPPED) {
			fprintf(stderr, "PPC: execution error at PC=%08X: %s\n",
			        GetPC(), uc_strerror(err));
		}
	}

	void Stop() {
		stopped = true;
		if (uc) uc_emu_stop(uc);
	}

	// -- Register access --

	uint32_t GetGPR(int reg) {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_0 + reg, &val);
		return val;
	}

	void SetGPR(int reg, uint32_t value) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_0 + reg, &value);
	}

	double GetFPR(int reg) {
		uint64_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_FPR0 + reg, &val);
		double d;
		memcpy(&d, &val, sizeof(d));
		return d;
	}

	void SetFPR(int reg, double value) {
		uint64_t val;
		memcpy(&val, &value, sizeof(val));
		if (uc) uc_reg_write(uc, UC_PPC_REG_FPR0 + reg, &val);
	}

	uint32_t GetPC() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_PC, &val);
		return val;
	}

	void SetPC(uint32_t pc) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_PC, &pc);
	}

	uint32_t GetLR() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_LR, &val);
		return val;
	}

	void SetLR(uint32_t lr) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_LR, &lr);
	}

	uint32_t GetCTR() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_CTR, &val);
		return val;
	}

	void SetCTR(uint32_t ctr) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_CTR, &ctr);
	}

	uint32_t GetCR() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_CR, &val);
		return val;
	}

	void SetCR(uint32_t cr) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_CR, &cr);
	}

	uint32_t GetXER() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_XER, &val);
		return val;
	}

	void SetXER(uint32_t xer) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_XER, &xer);
	}

	uint32_t GetMSR() {
		uint32_t val = 0;
		if (uc) uc_reg_read(uc, UC_PPC_REG_MSR, &val);
		return val;
	}

	void SetMSR(uint32_t msr) {
		if (uc) uc_reg_write(uc, UC_PPC_REG_MSR, &msr);
	}

	void SetSCHandler(SCHandler handler) {
		scHandler = std::move(handler);
	}

	void SetTraceCode(bool enable) {
		traceCode = enable;
		if (enable && uc && !codeHook) {
			uc_err err = uc_hook_add(uc, &codeHook, UC_HOOK_CODE,
			                         (void *)codeTraceHook_cb,
			                         nullptr, 1, 0);
			checkErr(err, "uc_hook_add(CODE trace)");
		}
	}

} // namespace PPC
