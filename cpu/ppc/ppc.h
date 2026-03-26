#ifndef __cpu_ppc_h__
#define __cpu_ppc_h__

#include <cstdint>
#include <functional>

namespace PPC {

	using SCHandler = std::function<void()>;

	// Initialize the PPC engine. Maps memory[0..memorySize) as the
	// emulated address space. memory must be page-aligned (4096 bytes)
	// and memorySize must be a multiple of 4096.
	void Init(uint8_t *memory, uint32_t memorySize);

	// Shut down the PPC engine and release resources.
	void Shutdown();

	// Execute PPC code starting at pc with TOC register r2 set to toc.
	// Runs until Stop() is called or PC becomes 0 (sentinel for "return
	// from top-level call").
	void Execute(uint32_t pc, uint32_t toc);

	// Stop execution. Safe to call from within an SCHandler callback.
	void Stop();

	// Register access -- GPR (r0-r31)
	uint32_t GetGPR(int reg);
	void     SetGPR(int reg, uint32_t value);

	// Register access -- FPR (f0-f31)
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

	// Condition register (full 32-bit CR)
	uint32_t GetCR();
	void     SetCR(uint32_t cr);

	// XER (fixed-point exception register)
	uint32_t GetXER();
	void     SetXER(uint32_t xer);

	// MSR (machine state register)
	uint32_t GetMSR();
	void     SetMSR(uint32_t msr);

	// Set the handler for sc instructions.
	void SetSCHandler(SCHandler handler);

	// Enable per-instruction trace output to stderr.
	// Activated by the existing --trace-cpu flag when running a PPC tool.
	void SetTraceCode(bool enable);
}

#endif
